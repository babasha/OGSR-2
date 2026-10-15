// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Skinned-mesh pass (STEP B sub-step 2). See vk_pass_skinned.h.
#include "stdafx.h"
extern int ps_r_core1_prio;   // r_core1_prio (vk_console_min.cpp, global scope)
#include "vk_render_view.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter

// CKinematics (children + LL_GetTransform_R/LL_BoneCount) â€” same compat trick as
// the skeleton wrapper TUs. Keep dxRender_Visual mapped (children is xr_vector<dxRender_Visual*>).
#define FBasicVisualH
#include "vk_FBasicVisual.h"            // pulls vk_SkeletonCompat.h (dxRender_Visual->vkRender_Visual, vk_Visual.h)
#include "CRender_Vulkan.h"
#include "../xrRender/SkeletonCustom.h" // CKinematics
#include "../xrRender/SkeletonAnimated.h" // CKinematicsAnimated (blend-count diag)

#include "vk_pass_skinned.h"
#include "vk_rt_dyn.h"                 // RTDyn::Wanted — every leaf pre-skins when a ray may hit it
#include "vk_pass_ssao.h"              // SSAOPass::GetNormalFormat â€” NPC normal G-buffer target
#include "vk_motionvec.h"             // VK::MotionVec::Format â€” RG16F motion target (skinned MV overlay)
#include "vk_pass_world.h"              // DynamicVisuals() / DynVisual
#include "vk_swapchain.h"              // Swapchain.m_Format / m_DepthFormat
#include "vk_scene_color.h"            // HDR scene target format
#include "vk_shaders.h"               // g_ShaderManager
#include "vk_buffer.h"                // CVulkanBuffer
#include "vk_world_material.h"        // WorldMaterial / WorldMaterialCache (set 1 = diffuse)
#include "HW_Vulkan.h"                // VulkanHW.m_Device
#include "vk_command_buffer.h"        // CommandManager.GetCurrentFrame() â€” in-flight slot
#include "vk_pipeline_cache.h"        // PipelineCache::GetCacheObject() â€” shared disk-backed cache
#include "vk_compute_util.h"          // VK::MakePipelineLayout / CreateComputePipeline
#include "../../xrCore/cpu_budget.h"       // CpuBudget::enabled / CyclesPerMs -- прибор [VK Bones/up]
#include "../../xrCore/xr_platform_time.h" // xrPlatform::TickCount64
#include <future>                     // r_bones_mt -- раздача CalculateBones по пулу TTAPI
#include "vk_gfx_pipeline.h"          // GfxPipelineBuilder
#include "vk_env_light.h"             // EnvLight â€” shared per-frame sun/hemi/ambient UBO (set 2)
#include "vk_shadow.h"                // ShadowMap::SphereVisible â€” caster culling
#include "vk_pass_lightcones.h"       // SynthCones::Submit â€” lightplanes-derived beam cones
#include "vk_profiler.h"              // VK_CPU_PROBE â€” pre-skinning CPU cost
#include "../../xr_3da/device.h"      // RV().mFullTransform_hud (HUD projection), dwFrame

#include <unordered_map>
#include "vk_rr_guides.h"      // VK::RRGuide — guide attachments on the world-scope skinned pipelines
#include "../../xrCore/pinned_worker.h"   // r_bones_mt 2: the skeletons on core 1
#include <atomic>

// Console cvar at GLOBAL scope â€” a block-scope extern inside namespace VK would mangle as
// VK::ps_r_vsm_npc_dist -> LNK2001 (same trick as vk_pass_shadow's externs).
extern float ps_r_vsm_npc_dist;   // VSM NPC shadow cull distance (m); 0 = no cull
extern int   ps_r_preskin;        // compute pre-skinning on/off (A/B)
extern int   ps_r_bones_mt;       // r_bones_mt: CalculateBones для всех скелетов кадра -- предпроходом по пулу TTAPI
extern float ps_r_bones_lod_dist; // r_bones_lod_dist: farther world skeletons get their bones every other frame (BonesLodSkip)
extern float ps_r_rtao_dist;      // RTAO view-distance cutoff (m) — skeletons past it stay out of the dynamic BLAS
extern float ps_r_rtao_radius;    // RTAO ray length (m)

// nfs4: R4's geometry-LOD knobs (vk_console_min.cpp): r__ssa_glod_start / r__ssa_glod_end / r__geometry_lod.
extern float ps_r__GLOD_ssa_start, ps_r__GLOD_ssa_end, ps_r__LOD;
extern int   ps_r_skin_lod;     // r_skin_lod: R4 sliding-window LOD of progressive skeletons
extern float ps_r_skin_lod_k;   // r_skin_lod_k: multiplier on the R4 SSA thresholds (Switch preset 4)
extern float ps_r_skin_st_lod;  // r_skin_st_lod: distance of the first LOD window of an ST leaf (Switch preset 6)
extern float ps_r_skin_lod_bg;  // r_skin_lod_bg: distance of the first background-built window (vkSkinLodWin; Switch preset 6)
extern float ps_r_skin_lod_wpn; // r_skin_lod_wpn: the same for weapons, with 2 deeper windows (Switch preset 1; 0 = the body ladder)
extern int   ps_r_skin_hud_lod; // r_skin_hud_lod: the first-person hands/weapon at a fixed LOD window (0 = full; read at model load too)
extern int   ps_r_hud_vsun;     // r_hud_vsun: the HUD hands/weapon take the sun shadow per vertex (skinned.vert), not 4 taps a pixel
extern float ps_r_skin_prepass_dist; // r_skin_prepass_dist: NPCs in the depth prepass up to this range (was the constant 60)
extern int   ps_r_hud_prepass;  // r_hud_prepass: the first-person hands/weapon into the prepass depth (early-Z for the world under them)
extern int   ps_r_skin_ubo;     // r_skin_ubo: bones from the dynamic-UBO window (skin_bones.glsl) instead of the SSBO
extern int ps_r_pipe_stats;          // r_pipe_stats: dump the lit pipeline's registers (PipeStats)

namespace VK {

namespace {
    bool                  s_inited     = false;
    bool                  s_failed     = false;
    VkPipelineLayout      s_layout     = VK_NULL_HANDLE;
    VkDescriptorSetLayout s_setLayout  = VK_NULL_HANDLE;
    VkDescriptorPool      s_pool       = VK_NULL_HANDLE;
    VkDescriptorSet       s_set        = VK_NULL_HANDLE;
    VkShaderModule        s_vs         = VK_NULL_HANDLE;
    VkShaderModule        s_fs         = VK_NULL_HANDLE;
    VkShaderModule        s_shadowVS   = VK_NULL_HANDLE;  // depth-only caster VS (optional)
    VkShaderModule        s_prepassVS  = VK_NULL_HANDLE;  // depth-prepass AT caster VS (optional)
    VkShaderModule        s_prepassFS  = VK_NULL_HANDLE;  // ... + alpha-test FS (matches skinned.frag)
    VkShaderModule        s_normalVS   = VK_NULL_HANDLE;  // NPC normal G-buffer VS (skins normal â†’ world, optional)
    VkShaderModule        s_normalFS   = VK_NULL_HANDLE;  // ... + AT FS writing worldN*0.5+0.5
    VkShaderModule        s_mvVS       = VK_NULL_HANDLE;  // motion-vector VS (skins cur+prev pose, optional)
    VkShaderModule        s_mvFS       = VK_NULL_HANDLE;  // ... â†’ RG16F screen motion
    VkShaderModule        s_mvPreVS    = VK_NULL_HANDLE;  // r_mv_prepass: pre-skinned cur+prev streams, no skinning
    VkPipelineLayout      s_mvLayout   = VK_NULL_HANDLE;  // set0 = bones + 152B push (cur/prev VP + bases + jitter)
    // r_skin_ubo twins (-DSKIN_BONES_UBO): the same passes reading bones[] from the dynamic-UBO window at
    // set 0 binding 1. Optional -- a missing twin keeps that pass on the SSBO.
    VkShaderModule        s_vsUbo        = VK_NULL_HANDLE;
    VkShaderModule        s_shadowVSUbo  = VK_NULL_HANDLE;
    VkShaderModule        s_prepassVSUbo = VK_NULL_HANDLE;
    VkShaderModule        s_normalVSUbo  = VK_NULL_HANDLE;
    CVulkanBuffer         s_boneSSBO;
    Fmatrix*              s_boneMapped = nullptr;
    std::unordered_map<u32, VkPipeline> s_pipelines;        // keyed by vertex stride (36/40/44)
    std::unordered_map<u32, VkPipeline> s_shadowPipelines;  // depth-only casters, same key
    std::unordered_map<u32, VkPipeline> s_prepassPipelines; // scene depth-prepass AT casters
    std::unordered_map<u32, VkPipeline> s_normalPipelines;  // NPC normal G-buffer casters, same key
    std::unordered_map<u32, VkPipeline> s_motionPipelines;  // motion-vector casters, same key
    // r_mv_prepass: depth-WRITING motion pipelines for the prepass. Keyed by stride
    // with bit31 = "the pre-skinned twin" (two vertex streams, no skinning).
    std::unordered_map<u32, VkPipeline> s_prepassMotionPipelines;

    // Per-skeleton bone-slot record for the motion pass: maps a CKinematics to the
    // absolute baseBone it occupied. Rolled each frame â€” SF().prevBoneMap then holds
    // LAST frame's slots, whose SSBO regions are still live (FRAMES_IN_FLIGHTâ‰¥2),
    // so the MV vertex shader can skin the previous pose for true animation motion.
    // Same, but for the first-person HUD skeletons (drawn with the HUD-FOV
    // projection at near depth) — the HUD MV overlay needs its own prev poses.

    // Per-frame upload registry: every skeleton whose bones landed in the SSBO
    // this frame. Bones are stored PRE-MULTIPLIED by the object's world matrix,
    // so S*pos in the shaders is world-space and one registry serves both the
    // shadow casters (mvp = lightVP) and the main pass (mvp = viewProj).
    // RENDER_THREAD_PLAN step 6b: what the record side used to read live from the CKinematics.
    // Both are filled by Skinned_UploadBones (PREPARE, extract side) right after the bones are final:
    //   sphere -- K->vis.sphere (recomputed by CalculateBones),
    //   beams  -- per synth-beam leaf: bone visibility + render transform (single-bone fans follow their bone).
    struct BeamBone { const void* child; bool visible; Fmatrix xf; };
    struct SkelUpload { CKinematics* K; Fmatrix xform; u32 baseBone; u16 boneCount; float hemi; bool noShadow;
                        Fsphere sphere{}; xr_vector<BeamBone> beams; };
    // (world dynamics / characters / first-person HUD / shadow-only: the lists live in SkinFrame below)
    // Casters the colour pass must not see: the actor's own model in first person
    // (g_actor_shadow), collected by g_hud->Render_SMAP. Shadow + VSM read it, the
    // main pass never does. See VK::ShadowOnlyVisuals().

    constexpr u32 kMaxBones       = 16384;   // bone-matrix slots PER frame-in-flight (~1 MB each)
    constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;

    // r_skin_ubo: the bone buffer is ALSO bound as a dynamic UBO (set 0 binding 1) over a kBoneWin-bone window --
    // 1024 x 64 B = 64 KB, the most one constant bank holds (= the BonesWin array in skin_bones.glsl). Windows
    // start every kBoneWinStep bones, so any skeleton of up to kBoneWin - kBoneWinStep bones sits whole inside the
    // window that starts at or below its base, and the skeletons uploaded after it share that window until the
    // next step: the set is rebound per WINDOW, not per draw. A bigger skeleton falls back to the SSBO pipeline.
    // The SSBO itself is padded by one window so the last one stays inside the buffer.
    constexpr u32 kBoneWin     = 1024;
    constexpr u32 kBoneWinStep = 512;
    static_assert(kBoneWin * sizeof(Fmatrix) == 65536, "one window = one 64 KB constant bank");
    // Pipeline-key bit meaning "the *_ubo.vert twin": ORed into the stride every pipeline getter is keyed by,
    // so the twins live in the same maps and the same teardown. Strides are 36/40/44; bit 31 is the prepass MV key.
    constexpr u32 kUboKey      = 1u << 30;
    constexpr u32 kNoWin       = u32(-1);   // the SSBO route: dynamic offset 0, absolute baseBone
    constexpr u32 kUnbound     = u32(-2);   // set 0 not bound yet in this loop

    struct BoneRoute { u32 key; u32 baseBone; u32 win; };
    // Which bones a draw reads: the UBO window when r_skin_ubo is on, the pass has its twin and the skeleton fits,
    // else the SSBO. `key` = the stride (| kUboKey) for the pipeline getter, `baseBone` = what the push carries.
    static BoneRoute RouteBones(u32 stride, u32 baseBone, u32 boneCount, VkShaderModule twin)
    {
        if (ps_r_skin_ubo && twin != VK_NULL_HANDLE) {
            const u32 win = (baseBone / kBoneWinStep) * kBoneWinStep;
            if (baseBone - win + boneCount <= kBoneWin) return { stride | kUboKey, baseBone - win, win };
        }
        return { stride, baseBone, kNoWin };
    }
    // Set 0 with the window's dynamic offset (0 for the SSBO route -- the SSBO pipelines ignore binding 1).
    static void BindBones(VkCommandBuffer cmd, VkPipelineLayout layout, u32 win)
    {
        const u32 off = (win == kNoWin) ? 0u : win * (u32)sizeof(Fmatrix);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &s_set, 1, &off);
    }

    // AO is short-range, so the depth prepass keeps only the NPCs near the camera as
    // occluders. ONE constant, because three walks now have to agree on it exactly:
    // the depth prepass, its motion-writing twin (r_mv_prepass), and the leftover MV
    // overlay that must pick up precisely the skeletons those two dropped.
    // 07-10: r_skin_prepass_dist (default 60 = the old constant): NPCs in the prepass cost their vertices a second time
    // (on the Switch the skinning is in every vertex shader), for early-Z on a few pixels. Read once per walk.
    // (declared at the top, outside the namespaces)

    // ---------------------------------------------------------------------
    // COMPUTE PRE-SKINNING (r_preskin) â€” see Skinned_PreSkin / preskin.comp.
    // ---------------------------------------------------------------------
    VkPipeline            s_psPipe    = VK_NULL_HANDLE;
    VkPipelineLayout      s_psLayout  = VK_NULL_HANDLE;
    VkDescriptorSetLayout s_psSetL    = VK_NULL_HANDLE;
    VkDescriptorPool      s_psPool    = VK_NULL_HANDLE;
    VkDescriptorSet       s_psSet     = VK_NULL_HANDLE;
    CVulkanBuffer         s_psBuf;                        // shared output pool (GPU-only)
    // PREVIOUS-frame world positions for the same vertices, same order (r_mv_prepass).
    // Bound as a second vertex stream by the motion-writing prepass pipeline, so one
    // vertexOffset indexes both pools. vec3 only — a motion vector needs no normals.
    CVulkanBuffer         s_psPrevBuf;
    bool                  s_psInited  = false;
    bool                  s_psFailed  = false;

    // Output layout = vertHW_1W (36 B). Chosen so the EXISTING skinned pipelines
    // consume it unchanged (see preskin.comp's header).
    constexpr u32 kPsStride     = 36;
    constexpr u32 kPsBytesSlot  = 12u << 20;              // per frame-in-flight (~349k verts)
    constexpr u32 kPsVertsSlot  = kPsBytesSlot / kPsStride;
    // Previous-pose pool: same vertex count, position only (12 B/vertex → a third of
    // the main pool). Sized from kPsVertsSlot so the two can never disagree about
    // which vertex a cursor means.
    constexpr u32 kPsPrevStride    = 12;
    constexpr u32 kPsPrevBytesSlot = kPsVertsSlot * kPsPrevStride;

    // Must match preskin.comp's push block. The 8-byte device address goes first
    // so the struct needs no padding.
    struct PreSkinPush { VkDeviceAddress src; u32 vertCount, srcStrideDW, dstFirst, skinMode, baseBone, boneCount, prevBase; };
    static_assert(sizeof(PreSkinPush) == 40, "must match preskin.comp PC block");

    // This frame's pool residency: leaf visual -> first vertex in the pool.
    // Rebuilt from scratch every frame; empty when r_preskin is 0, so every draw
    // site falls back to the classic per-pass skinning with no extra branch.
    // The pool's device address (AS build input for vk_rt_dyn) and this frame's
    // list of leaves a ray may hit: WORLD skeletons only — the first-person HUD
    // would occlude everything in front of the camera, and the shadow-only actor
    // body is never in the colour pass either.
    VkDeviceAddress s_psPoolAddr = 0;
    // ⭐РЕШЕНИЕ, ОТДЕЛЁННОЕ ОТ ЗАПИСИ (19-09-2026). Раньше цикл по листьям сразу
    // писал vkCmdPushConstants+vkCmdDispatch и ТУТ ЖЕ наполнял SF().psMap, который
    // читает каждый рисующий скиннед-пасс. Пока это одно тело, PreSkin на одном
    // потоке и SunShadow/World на другом -- гонка по построению (это и был краш
    // `Pass_SunShadow -> Skinned_UploadBones -> std::_Hash`). Теперь цикл живёт в
    // Skinned_PreSkinPrepare (фаза prepare, до любой записи), а тело пасса лишь
    // проигрывает готовый список. См. vk_pass_registry.h.
    struct PreSkinJob { PreSkinPush pc; u32 groups; };

    // TWO-SLOT PIPELINE: everything Skinned_UploadBones / Skinned_PreSkinPrepare (PREPARE, main thread) produces and the
    // record passes read, per frame slot. SF() picks by role: extract writes the slot of the frame it is extracting, the
    // render thread reads the slot of the frame it is recording.
    //   prevBoneMap* are a COPY of the previous frame's cur* taken at extract: the other slot's cur* is rewritten by the
    //   NEXT extract while this frame's record still reads it as the previous pose.
    using BoneMap = std::unordered_map<CKinematics*, std::pair<u32, u32>>;   // K -> {baseBone, boneCount}
    struct SkinFrame {
        xr_vector<SkelUpload>      uploads;        // world dynamics
        xr_vector<SkelUpload>      uploadsHud;     // first-person HUD (never casts)
        xr_vector<SkelUpload>      uploadsShadow;  // casters the colour pass must not see (see ShadowOnlyVisuals())
        xr_vector<CharacterBounds> characters;     // see Skinned_CharacterBounds
        BoneMap curBoneMap, prevBoneMap, curBoneMapHud, prevBoneMapHud;
        u32 uploadFrame = u32(-1);
        std::unordered_map<const void*, u32> psMap;   // leaf visual -> first vertex in the pool
        xr_vector<PreSkinRtLeaf> psRtLeaves;
        xr_vector<PreSkinJob> psJobs;
        u32 psFrame = u32(-1);
        u32 identityBone = 0;                      // bone slot holding IDENTITY for this frame's region
    };
    RT::PerSlot<SkinFrame> s_skinSlots;
    inline SkinFrame& SF() { return s_skinSlots.cur(); }
    // (SF().psMap / SF().psRtLeaves / SF().psJobs / psFrame / identityBone live in SkinFrame below)
    u32 s_psLeaves = 0, s_psVerts = 0, s_psPeak = 0, s_psOverflow = 0;
    u32 s_psSkipped1W = 0, s_psMaxBones = 0;   // diag: how prop-heavy is this scene?

    // Where a leaf's vertices come from THIS frame. Pre-skinned leaves are drawn
    // as 1-weight geometry against the identity bone (the blend already happened
    // in compute); everything else keeps the real bones and its own vertHW stride.
    struct DrawSrc { VkBuffer vb; s32 firstVertex; u32 stride; u32 skinMode; u32 baseBone; u32 boneCount; };

    static DrawSrc ResolveDrawSrc(const void* leaf, const VK_Render_Mesh* mesh, u16 rmode,
                                  u32 baseBone, u32 boneCount)
    {
        // The frame check is not paranoia: the pool region is picked by the
        // in-flight slot, so a map left over from an earlier frame would point a
        // draw at another slot's vertices. Any frame where the PreSkin pass did
        // not run therefore falls through to the classic path.
        if (SF().psFrame == RV().frameIndex && !SF().psMap.empty()) {
            auto it = SF().psMap.find(leaf);
            if (it != SF().psMap.end())
                return { s_psBuf.GetHandle(), (s32)it->second, kPsStride, 1u, SF().identityBone, 1u };
        }
        return { mesh->p_rm_Vertices->GetHandle(), (s32)mesh->vBase, mesh->vStride,
                 (rmode <= 2u) ? 1u : (u32(rmode) - 1u), baseBone, boneCount };
    }
    // Set 2 (per-frame sun/hemi/ambient) is the shared EnvLight set â€” see vk_env_light.{h,cpp}.

    // Push block â€” must match skinned.{vert,frag}.glsl PushConstants. hudMode (read
    // by the fragment) flags first-person HUD so it gets sun-direction-independent light.
    struct SkinPush { Fmatrix mvp; u32 skinMode; u32 baseBone; u32 boneCount; float hudMode; float hemi; };

    // Motion-vector push â€” must match motion_vec_skinned.vert. 152 B (device max is
    // 256 here, see vk_pipeline.cpp). curVP/prevVP are UNJITTERED and project the
    // cur/prev poses (jitter-free MV); jitter re-applies the sub-pixel offset to
    // gl_Position only, so depth still bit-matches the jittered forward geometry.
    struct MVSkinPush { Fmatrix curVP; Fmatrix prevVP; u32 skinMode; u32 baseBone; u32 prevBase; u32 boneCount; float jitterX; float jitterY; };
    static_assert(sizeof(MVSkinPush) == 152, "must match motion_vec_skinned.vert PC block");

    // Vertex input for the vertHW_* layouts. loc0 pos FLOAT4; loc1/3/4 packed
    // normal/tangent/binormal (UBYTE4N); loc2 = FLOAT2 (36/40) or FLOAT4 (44);
    // loc5 = 4 bone indices (40 only, dummy @0 elsewhere â€” unused by those modes).
    static void BuildSkinnedVI(u32 stride, VkVertexInputBindingDescription& b,
                               VkVertexInputAttributeDescription attrs[6])
    {
        b = { 0, stride, VK_VERTEX_INPUT_RATE_VERTEX };
        attrs[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0  };
        attrs[1] = { 1, 0, VK_FORMAT_R8G8B8A8_UNORM,      16 };
        attrs[3] = { 3, 0, VK_FORMAT_R8G8B8A8_UNORM,      20 };
        attrs[4] = { 4, 0, VK_FORMAT_R8G8B8A8_UNORM,      24 };
        if (stride == 44) {
            attrs[2] = { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 28 };  // tc + 2 bone idx
            attrs[5] = { 5, 0, VK_FORMAT_R8G8B8A8_UNORM,       0 };  // unused (2W/3W)
        } else if (stride == 40) {
            attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,  28 };       // tc only
            attrs[5] = { 5, 0, VK_FORMAT_R8G8B8A8_UNORM, 36 };       // 4 bone idx (4W)
        } else { // 36 (1W)
            attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,  28 };       // tc only
            attrs[5] = { 5, 0, VK_FORMAT_R8G8B8A8_UNORM, 0  };       // unused (1W)
        }
    }

    VkShaderModule s_glassDistFS = VK_NULL_HANDLE;   // glass_distort_skinned.frag (variant 3)

    // Pipeline variants: 0 = lit opaque, 1 = additive unlit (collimator marks),
    // 2 = GLASS (lit, src-alpha blend, no depth write â€” kinematics furniture/door
    // panes; the opaque path rendered them as solid texture), 3 = GLASS DISTORT
    // (same panes re-drawn into the heat-haze RT = refraction, r_glass_refr), 4 = LATE
    // GLASS (variant 2 for the "Glass" pass scope: BeginOverlayRendering has one colour
    // attachment, no RR guides and no shading-rate image).
    // The skinned FS carries the world's frame variants (IBL / debug / lamp shadows / RR
    // guides, shaders/world_variants.glsl) and its own path constant: the world half
    // (NPCs, props) or the first-person HUD half (hands/weapon) -- each pipeline compiles
    // only its half, so neither pays the other's registers. specBits = WS_IBL|WS_DEBUG|
    // WS_LAMPSH of the frame.
    static VkPipeline CreatePipeline(u32 key, u32 variant, u8 specBits, bool hud)
    {
        const bool ubo      = (key & kUboKey) != 0;   // r_skin_ubo twin
        const u32  stride   = key & ~kUboKey;
        const bool additive = (variant == 1u);
        const bool late     = (variant == 4u);
        const bool glass    = (variant == 2u) || late;
        const bool distort  = (variant == 3u);
        if (distort && s_glassDistFS == VK_NULL_HANDLE) {
            s_glassDistFS = g_ShaderManager->Load("glass_distort_skinned.frag.spv");
            if (s_glassDistFS == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        }
        VkVertexInputBindingDescription   binding{};
        VkVertexInputAttributeDescription attrs[6]{};
        BuildSkinnedVI(stride, binding, attrs);

        // Variant 3 renders into the particle heat-haze RT (RGBA8), not the scene.
        VK::GfxPipelineBuilder b(s_layout);
        VkBool32                 specVals[PipelineCache::kWorldSpecCount + 1];
        VkSpecializationMapEntry specEntries[PipelineCache::kWorldSpecCount + 1];
        VkSpecializationInfo     specInfo{};
        if (!distort) {
            // RR guides are written by the lit opaque variant only (PipelineTargets below).
            PipelineCache::FillWorldSpecInfo(specBits, variant == 0u && VK::RRGuide::Ready(), specVals, specEntries, specInfo);
            const u32 n = PipelineCache::kWorldSpecCount;
            specVals[n]    = hud ? 2u : 1u;   // SPEC_SKIN_PATH (int): 1 = world, 2 = HUD
            specEntries[n] = { 9u, n * (u32)sizeof(VkBool32), sizeof(VkBool32) };
            specInfo.mapEntryCount = n + 1;
            specInfo.dataSize      = (n + 1) * sizeof(VkBool32);
        }
        b.Vert(ubo ? s_vsUbo : s_vs).Frag(distort ? s_glassDistFS : s_fs, distort ? nullptr : &specInfo)
         .Bindings(&binding, 1).Attrs(attrs, 6)
         .Cull(VK_CULL_MODE_NONE)
         // marks/glass don't write depth (R4 zb(true,false))
         .Depth(true, !(additive || glass || distort))
         .Color(distort ? VK_FORMAT_R8G8B8A8_UNORM : VK::SceneColor::Format());
        // Variants 0/1/2 draw INSIDE the world colour scope, so when Ray
        // Reconstruction is on that scope carries four attachments and these
        // pipelines have to declare all four. write=false for now: the skinned FS
        // has not been converted to emit guides yet, so NPC pixels keep the
        // frame-start default. Variant 3 renders into the haze RT — untouched.
        // ⭐Variant 0 (lit opaque) now WRITES the guides: NPCs and — far more
        // numerous indoors — every kinematics prop go through it. Variants 1
        // (additive collimator marks) and 2 (glass) only declare the formats with a
        // zero write mask: both composite ON TOP of an already-shaded pixel, so
        // their colour is not the material RR has to demodulate.
        if (!distort && !late) VK::RRGuide::PipelineTargets(b, variant == 0u);
        b.DepthTarget(Swapchain.m_DepthFormat);
        if (additive) {
            // Collimator marks (R4 hud_reddotsight: blend(srcalpha, one)) â€”
            // the dot ADDS light over the sight glass.
            b.BlendAdd();
        }
        if (glass || distort) {
            // Translucent pane over whatever is behind (lit output, capped
            // alpha comes from the fragment â€” skinMode bit 32). The distort
            // variant blends the wobble over the haze RT's neutral 0.5.
            b.BlendAlpha();
        }

        // VRS: variants 0/1/2 draw inside the world-color pass with the SRI
        // attached — STATIC {1x1, KEEP, REPLACE} state so distant NPCs coarse-
        // shade with the world (static, not dynamic: see vk_pipeline_cache.cpp —
        // a dynamic rate gets invalidated by unrelated binds). The distort
        // variant renders into the haze RT (no SRI) and stays full-rate.
        VkPipelineFragmentShadingRateStateCreateInfoKHR fsrState{ VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR };
        fsrState.fragmentSize   = { 1, 1 };
        fsrState.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
        fsrState.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;   // SRI attachment wins
        if (VulkanHW.m_bVRSSupported && !distort && !late)
            b.RenderingNext(&fsrState).CreateFlags(VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR);

        VkPipeline h = b.Build("Skinned stride=%u variant=%u ubo=%d", stride, variant, ubo ? 1 : 0);
        if (h == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        Msg("[VK Skinned] pipeline created stride=%u variant=%u ubo=%d", stride, variant, ubo ? 1 : 0);
        if (ps_r_pipe_stats > 0 && variant == 0u)
            VK::PipeStats::Dump(h, "skinned lit stride=%u %s%s", stride, hud ? "HUD" : "world",
                                (specBits & PipelineCache::WS_LAMPSH) ? " +lampsh" : "");
        return h;
    }

    // `stride` may carry kUboKey (RouteBones): the r_skin_ubo twin, cached beside the SSBO one.
    // hud = the draw is the first-person HUD (hands/weapon) -> the HUD half of the FS.
    static VkPipeline GetPipeline(u32 stride, u32 variant = 0u, bool hud = false)
    {
        // Bit 24 = RR guide attachments (see PipelineCache::Key::rrGuides) — the
        // attachment list differs, so a cached pipeline from the other state is invalid.
        const bool distort = variant == 3u;           // glass_distort_skinned.frag: no variants
        using namespace PipelineCache;
        // + WS_LAMPSH whenever a lamp has a map or cookie: the frame mask drops it while the world reads the torch from
        // the sun mask (r_flash_mask), which NPCs -- outside the prepass it is built from -- cannot.
        const u8 frame = u8(GetFrameSpecMask() | (EnvLight::SkinnedLampShadows() ? WS_LAMPSH : 0u));
        const bool hudPath = hud && !distort;
        // The HUD half reads neither IBL nor lamps: only the debug bit can change its code.
        const u8 bits  = distort ? u8(0) : u8(frame & (hudPath ? WS_DEBUG : (WS_IBL | WS_DEBUG | WS_LAMPSH)));
        const u32 key = stride | (variant << 16) | (VK::RRGuide::Ready() ? (1u << 24) : 0u)
                      | ((bits & WS_IBL) ? (1u << 26) : 0u) | ((bits & WS_DEBUG) ? (1u << 27) : 0u)
                      | ((bits & WS_LAMPSH) ? (1u << 28) : 0u) | (hudPath ? (1u << 29) : 0u);
        auto it = s_pipelines.find(key);
        if (it != s_pipelines.end()) return it->second;
        VkPipeline p = CreatePipeline(stride, variant, bits, hudPath);
        s_pipelines.emplace(key, p);
        // The torch going on flips WS_LAMPSH: build that twin now, with this one, so the first
        // click of the flashlight near an NPC doesn't compile a skinned pipeline mid-game.
        if (!distort && !hudPath) {
            const u32 twin = key ^ (1u << 28);
            if (s_pipelines.find(twin) == s_pipelines.end())
                s_pipelines.emplace(twin, CreatePipeline(stride, variant, u8(bits ^ WS_LAMPSH), false));
        }
        return p;
    }

    static bool Init()
    {
        if (s_inited) return !s_failed;
        s_inited = true;

        if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
        s_vs = g_ShaderManager->Load("skinned.vert.spv");
        s_fs = g_ShaderManager->Load("skinned.frag.spv");   // canonical name on every platform (was skinned_v3 on the Switch SD card)
        if (s_vs == VK_NULL_HANDLE || s_fs == VK_NULL_HANDLE) {
            Msg("![VK Skinned] failed to load skinned.vert/frag.spv");
            s_failed = true; return false;
        }
        // Optional â€” absence just disables NPC shadow casting.
        s_shadowVS = g_ShaderManager->Load("shadow_skinned.vert.spv");
        if (s_shadowVS == VK_NULL_HANDLE)
            Msg("![VK Skinned] shadow_skinned.vert.spv missing â€” skinned shadow casting disabled");
        // Optional â€” absence just keeps NPCs out of the depth prepass (no NPC AO).
        s_prepassVS = g_ShaderManager->Load("shadow_skinned_at.vert.spv");
        s_prepassFS = g_ShaderManager->Load("shadow_skinned_at.frag.spv");
        if (s_prepassVS == VK_NULL_HANDLE || s_prepassFS == VK_NULL_HANDLE)
            Msg("![VK Skinned] shadow_skinned_at.{vert,frag}.spv missing â€” NPCs excluded from the depth prepass");
        // Optional â€” absence just keeps NPCs on depth-derived GTAO normals (no NPC normal G-buffer).
        s_normalVS = g_ShaderManager->Load("shadow_skinned_normal.vert.spv");
        s_normalFS = g_ShaderManager->Load("shadow_skinned_normal.frag.spv");
        if (s_normalVS == VK_NULL_HANDLE || s_normalFS == VK_NULL_HANDLE)
            Msg("![VK Skinned] shadow_skinned_normal.{vert,frag}.spv missing â€” NPC AO normals disabled");
        // Optional â€” absence just keeps NPCs on camera-only motion vectors (no animation MV).
        s_mvVS = g_ShaderManager->Load("motion_vec_skinned.vert.spv");
        s_mvFS = g_ShaderManager->Load("motion_vec_skinned.frag.spv");
        if (s_mvVS == VK_NULL_HANDLE || s_mvFS == VK_NULL_HANDLE)
            Msg("![VK Skinned] motion_vec_skinned.{vert,frag}.spv missing â€” NPC animation motion vectors disabled");
        // r_mv_prepass twin for PRE-SKINNED leaves: no skinning at all, the two poses
        // arrive as two vertex streams (see skin_prepass_mv.vert). Absence just keeps
        // NPCs on the overlay path.
        s_mvPreVS = g_ShaderManager->Load("skin_prepass_mv.vert.spv");
        if (s_mvPreVS == VK_NULL_HANDLE)
            Msg("![VK Skinned] skin_prepass_mv.vert.spv missing â€” NPC motion stays on the overlay raster");

        // r_skin_ubo twins (-DSKIN_BONES_UBO). Optional: a missing one keeps its pass on the SSBO.
        s_vsUbo        = g_ShaderManager->Load("skinned_ubo.vert.spv");
        s_shadowVSUbo  = s_shadowVS  ? g_ShaderManager->Load("shadow_skinned_ubo.vert.spv")        : VK_NULL_HANDLE;
        s_prepassVSUbo = s_prepassVS ? g_ShaderManager->Load("shadow_skinned_at_ubo.vert.spv")     : VK_NULL_HANDLE;
        s_normalVSUbo  = s_normalVS  ? g_ShaderManager->Load("shadow_skinned_normal_ubo.vert.spv") : VK_NULL_HANDLE;
        Msg("[VK Skinned] r_skin_ubo twins: colour %d shadow %d prepass %d normal %d",
            s_vsUbo ? 1 : 0, s_shadowVSUbo ? 1 : 0, s_prepassVSUbo ? 1 : 0, s_normalVSUbo ? 1 : 0);

        // Descriptor set layout, VERTEX stage: 0 = the bone matrices as an SSBO (absolute slots), 1 = the same
        // buffer as a dynamic UBO, one kBoneWin window per bind (r_skin_ubo). Every bind passes one dynamic offset:
        // the loops below explicitly (BindBones), the motion-vector passes through the vk_dyn_sets.h wrapper (0).
        if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC }, 1,
                                    s_setLayout, s_pool, &s_set,
                                    VK_SHADER_STAGE_VERTEX_BIT, "Skinned.Bones")) {
            s_failed = true; return false;
        }

        // Set 2 = shared per-frame env lighting (sun/hemi/ambient). Owned by EnvLight;
        // ensure it's initialised so its set layout exists for the pipeline layout.
        EnvLight::Init();
        VkDescriptorSetLayout lightLayout = EnvLight::GetSetLayout();
        if (lightLayout == VK_NULL_HANDLE) { Msg("![VK Skinned] EnvLight set layout not ready"); s_failed = true; return false; }

        // Pipeline layout: push {mvp, skinMode, baseBone} (VERTEX) + set0=bone SSBO +
        // set1=material (base/detail/lmap) so the FS can sample the diffuse texture +
        // set2=per-frame environment lighting (FRAGMENT).
        VkDescriptorSetLayout matLayout = WorldMaterialCache::GetSetLayout();
        if (matLayout == VK_NULL_HANDLE) { Msg("![VK Skinned] material set layout not ready"); s_failed = true; return false; }
        VkDescriptorSetLayout setLayouts[3] = { s_setLayout, matLayout, lightLayout };
        VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkinPush) };
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 3; plci.pSetLayouts = setLayouts;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_layout) != VK_SUCCESS) { s_failed = true; return false; }

        // Motion-vector pipeline layout: set 0 (bone SSBO) + set 1 (diffuse, for the
        // alpha-test) + a 144-byte VERTEX push (cur/prev VP + bone bases). No light set
        // â€” the MV fragment only needs the two clip positions + the cutout test.
        if (s_mvVS != VK_NULL_HANDLE && s_mvFS != VK_NULL_HANDLE) {
            VkDescriptorSetLayout mvSets[2] = { s_setLayout, matLayout };
            VkPushConstantRange mvPcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(MVSkinPush) };
            VkPipelineLayoutCreateInfo mvPlci{};
            mvPlci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            mvPlci.setLayoutCount = 2; mvPlci.pSetLayouts = mvSets;
            mvPlci.pushConstantRangeCount = 1; mvPlci.pPushConstantRanges = &mvPcr;
            if (vkCreatePipelineLayout(VulkanHW.m_Device, &mvPlci, nullptr, &s_mvLayout) != VK_SUCCESS) {
                Msg("![VK Skinned] motion-vector pipeline layout failed â€” NPC animation MV disabled");
                s_mvVS = s_mvFS = VK_NULL_HANDLE;   // disable the MV path, keep the rest alive
            }
        }

        // Bone SSBO (host-visible, persistently mapped â€” Create sets HOST_ACCESS+MAPPED for STORAGE).
        // Sized FRAMES_IN_FLIGHT Ã— kMaxBones: each in-flight frame owns region
        // [slot*kMaxBones, (slot+1)*kMaxBones). Frame S only writes its region after
        // WaitForFence(S) in CRender::Begin proved the GPU finished the previous frame-S
        // submission that read it, so the regions still in flight (S+1/S+2) are never
        // clobbered. baseBone push-constant absolutely-indexes the whole buffer, so the
        // single VK_WHOLE_SIZE descriptor below needs no per-frame rebind.
        // + one kBoneWin of padding: the window that starts in the last slot region reaches past it (r_skin_ubo),
        // and a dynamic UBO's offset + range must stay inside the buffer. Nothing is ever written there.
        s_boneSSBO.Create(VkDeviceSize(kMaxBones * kFramesInFlight + kBoneWin) * sizeof(Fmatrix),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        s_boneMapped = static_cast<Fmatrix*>(s_boneSSBO.Map());
        if (!s_boneMapped) { Msg("![VK Skinned] bone SSBO map failed"); s_failed = true; return false; }

        VK::DescriptorWriter(s_set)
            .StorageBuffer(0, s_boneSSBO.GetHandle(), VkDeviceSize(kMaxBones) * kFramesInFlight * sizeof(Fmatrix))
            .Buffer(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, { s_boneSSBO.GetHandle(), 0, VkDeviceSize(kBoneWin) * sizeof(Fmatrix) })
            .Flush();
        VK::DynSets::Register(s_set, 1);   // binds that pass no offset (the MV passes) get 0

        Msg("[VK Skinned] init OK (SSBO %u bones, push=%u)", kMaxBones, (u32)sizeof(SkinPush));
        return true;
    }

    // Depth-only caster pipeline (sun shadow map): shadow_skinned.vert skins the
    // vertex and writes light-clip depth; no fragment stage, no color. Reuses
    // s_layout (sets 1/2 simply go unbound â€” the shader only touches set 0).
    static VkPipeline CreateShadowPipeline(u32 key)
    {
        const bool ubo    = (key & kUboKey) != 0;   // r_skin_ubo twin
        const u32  stride = key & ~kUboKey;
        VkVertexInputBindingDescription   binding{};
        VkVertexInputAttributeDescription attrs[6]{};
        BuildSkinnedVI(stride, binding, attrs);

        // VERTEX only, no color attachments — depth-only pass.
        VkPipeline h = VK::GfxPipelineBuilder(s_layout)
            .Vert(ubo ? s_shadowVSUbo : s_shadowVS)
            .Bindings(&binding, 1).Attrs(attrs, 6)
            .Cull(VK_CULL_MODE_NONE)
            .DynamicDepthBias()                  // caller sets the same bias as statics
            .Depth(true, true)
            .DepthTarget(VK_FORMAT_D32_SFLOAT)   // matches the shadow map
            .Build("Skinned shadow stride=%u ubo=%d", stride, ubo ? 1 : 0);
        if (h == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        Msg("[VK Skinned] shadow pipeline created stride=%u ubo=%d", stride, ubo ? 1 : 0);
        return h;
    }

    static VkPipeline GetShadowPipeline(u32 stride)
    {
        auto it = s_shadowPipelines.find(stride);
        if (it != s_shadowPipelines.end()) return it->second;
        VkPipeline p = CreateShadowPipeline(stride);
        s_shadowPipelines.emplace(stride, p);
        return p;
    }

    // Depth-PREPASS caster pipeline: skinned skinning + alpha-test fragment
    // (same a<0.25 discard as the color pass â†’ identical coverage). Renders
    // into the scene depth, so no depth bias and the scene depth format.
    static VkPipeline CreatePrepassPipeline(u32 key, bool mv)
    {
        const bool ubo    = (key & kUboKey) != 0;   // r_skin_ubo twin
        const u32  stride = key & ~kUboKey;
        VkVertexInputBindingDescription   binding{};
        VkVertexInputAttributeDescription attrs[6]{};
        BuildSkinnedVI(stride, binding, attrs);

        VK::GfxPipelineBuilder b(s_layout);   // no color attachments (mv: one, masked off)
        b.Vert(ubo ? s_prepassVSUbo : s_prepassVS).Frag(s_prepassFS)
         .Bindings(&binding, 1).Attrs(attrs, 6)
         .Cull(VK_CULL_MODE_NONE)
         .Depth(true, true)
         .DepthTarget(Swapchain.m_DepthFormat);   // scene depth (prepass gates on D32)
        // r_mv_prepass: the prepass scope carries the RG16F motion attachment, and
        // dynamic rendering makes the attachment list part of the pipeline. NPCs do
        // NOT write it here yet — their motion still comes from the overlay in
        // MotionVecDyn, which reaches this field afterwards — so the mask is 0 and
        // the sentinel survives for the resolve to reconstruct.
        if (mv) b.Color(VK::MotionVec::Format(), 0);
        VkPipeline h = b.Build("Skinned prepass stride=%u mv=%d ubo=%d", stride, mv ? 1 : 0, ubo ? 1 : 0);
        if (h == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        Msg("[VK Skinned] prepass pipeline created stride=%u mv=%d ubo=%d", stride, mv ? 1 : 0, ubo ? 1 : 0);
        return h;
    }

    // Two getters rather than one with a flag: RenderSkinnedCasters takes the
    // pipeline chooser as a plain function pointer, and a hidden mode variable read
    // from inside it is exactly the kind of implicit state this pass has been bitten
    // by before. The MV twin keys on stride | bit31, sharing the map + teardown.
    static VkPipeline GetPrepassPipelineKeyed(u32 stride, bool mv)
    {
        const u32 key = stride | (mv ? 0x80000000u : 0u);
        auto it = s_prepassPipelines.find(key);
        if (it != s_prepassPipelines.end()) return it->second;
        VkPipeline p = CreatePrepassPipeline(stride, mv);
        s_prepassPipelines.emplace(key, p);
        return p;
    }
    static VkPipeline GetPrepassPipeline(u32 stride)   { return GetPrepassPipelineKeyed(stride, false); }
    static VkPipeline GetPrepassPipelineMV(u32 stride) { return GetPrepassPipelineKeyed(stride, true);  }

    // NPC NORMAL G-buffer pipeline: same skinning as the depth prepass (positions
    // bit-match â†’ LEQUAL passes only on the visible surface), but writes the
    // world-space normal to a color attachment and does NOT write depth (tests
    // against the already-laid prepass depth). Feeds GTAO real NPC normals.
    static VkPipeline CreateNormalPipeline(u32 key)
    {
        const bool ubo    = (key & kUboKey) != 0;   // r_skin_ubo twin
        const u32  stride = key & ~kUboKey;
        VkVertexInputBindingDescription   binding{};
        VkVertexInputAttributeDescription attrs[6]{};
        BuildSkinnedVI(stride, binding, attrs);

        VkPipeline h = VK::GfxPipelineBuilder(s_layout)
            .Vert(ubo ? s_normalVSUbo : s_normalVS).Frag(s_normalFS)
            .Bindings(&binding, 1).Attrs(attrs, 6)
            .Cull(VK_CULL_MODE_NONE)
            .Depth(true, false)                     // test only â€” the prepass owns the depth
            .Color(SSAOPass::GetNormalFormat())
            .DepthTarget(Swapchain.m_DepthFormat)   // tests the scene prepass depth
            .Build("Skinned normal stride=%u ubo=%d", stride, ubo ? 1 : 0);
        if (h == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        Msg("[VK Skinned] normal G-buffer pipeline created stride=%u ubo=%d", stride, ubo ? 1 : 0);
        return h;
    }

    static VkPipeline GetNormalPipeline(u32 stride)
    {
        auto it = s_normalPipelines.find(stride);
        if (it != s_normalPipelines.end()) return it->second;
        VkPipeline p = CreateNormalPipeline(stride);
        s_normalPipelines.emplace(stride, p);
        return p;
    }

    // MOTION-VECTOR pipeline: skins cur+prev pose (motion_vec_skinned.vert) and
    // writes RG16F screen motion. Tests against the already-laid scene depth
    // (LEQUAL, no write) so only the visible NPC surface overwrites the static
    // camera field. Negative-height viewport (set by the caller) matches the
    // forward/prepass raster so positions/depth bit-match â†’ LEQUAL passes on equal.
    static VkPipeline CreateMotionPipeline(u32 stride)
    {
        VkVertexInputBindingDescription   binding{};
        VkVertexInputAttributeDescription attrs[6]{};
        BuildSkinnedVI(stride, binding, attrs);

        VkPipeline h = VK::GfxPipelineBuilder(s_mvLayout)
            .Vert(s_mvVS).Frag(s_mvFS)
            .Bindings(&binding, 1).Attrs(attrs, 6)
            .Cull(VK_CULL_MODE_NONE)
            .Depth(true, false)   // scene depth already owns the surface
            .Color(VK::MotionVec::Format(), VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT)   // RG16F motion
            .DepthTarget(Swapchain.m_DepthFormat)
            .Build("Skinned motion stride=%u", stride);
        if (h == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        Msg("[VK Skinned] motion-vector pipeline created stride=%u", stride);
        return h;
    }

    static VkPipeline GetMotionPipeline(u32 stride)
    {
        auto it = s_motionPipelines.find(stride);
        if (it != s_motionPipelines.end()) return it->second;
        VkPipeline p = CreateMotionPipeline(stride);
        s_motionPipelines.emplace(stride, p);
        return p;
    }

    // r_mv_prepass: the motion pipeline that also LAYS THE DEPTH. Same layout, same
    // fragment shader (alpha-test + RG16F motion), depth WRITE on — this raster IS the
    // NPC's depth prepass, so the overlay in MotionVecDyn is not needed at all.
    //
    // Two vertex-input shapes, because the prepass has two vertex sources:
    //   preskinned = the compute pool (36 B world-space) + the parallel previous-pose
    //     pool (12 B). No skinning in the shader — the current position must be READ,
    //     not recomputed, or its depth lands one ULP off the colour pass's LEQUAL.
    //   classic    = the mesh's own vertHW_* buffer, skinned twice in the vertex
    //     shader (motion_vec_skinned.vert). That is what the colour pass does for
    //     these same leaves, so their depths agree by construction.
    static VkPipeline CreatePrepassMotionPipeline(u32 stride, bool preskinned)
    {
        VK::GfxPipelineBuilder b(s_mvLayout);
        VkVertexInputBindingDescription   bindings[2]{};
        VkVertexInputAttributeDescription attrs[6]{};
        if (preskinned) {
            if (s_mvPreVS == VK_NULL_HANDLE) return VK_NULL_HANDLE;
            bindings[0] = { 0, kPsStride,     VK_VERTEX_INPUT_RATE_VERTEX };
            bindings[1] = { 1, kPsPrevStride, VK_VERTEX_INPUT_RATE_VERTEX };
            attrs[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0  };   // pool: world pos (w = 1)
            attrs[1] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,       28 };   // pool: leaf UV (dwords 7,8)
            attrs[2] = { 6, 1, VK_FORMAT_R32G32B32_SFLOAT,    0  };   // prev pool: previous world pos
            b.Vert(s_mvPreVS).Frag(s_mvFS).Bindings(bindings, 2).Attrs(attrs, 3);
        } else {
            if (s_mvVS == VK_NULL_HANDLE) return VK_NULL_HANDLE;
            BuildSkinnedVI(stride, bindings[0], attrs);
            b.Vert(s_mvVS).Frag(s_mvFS).Bindings(bindings, 1).Attrs(attrs, 6);
        }
        VkPipeline h = b.Cull(VK_CULL_MODE_NONE)
            .Depth(true, true)                     // THIS raster lays the NPC depth
            .Color(VK::MotionVec::Format(), VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT)
            .DepthTarget(Swapchain.m_DepthFormat)
            .Build("Skinned prepass MV stride=%u preskinned=%d", stride, preskinned ? 1 : 0);
        if (h == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        Msg("[VK Skinned] prepass MV pipeline created stride=%u preskinned=%d", stride, preskinned ? 1 : 0);
        return h;
    }

    static VkPipeline GetPrepassMotionPipeline(u32 stride, bool preskinned)
    {
        const u32 key = stride | (preskinned ? 0x80000000u : 0u);
        auto it = s_prepassMotionPipelines.find(key);
        if (it != s_prepassMotionPipelines.end()) return it->second;
        VkPipeline p = CreatePrepassMotionPipeline(stride, preskinned);
        s_prepassMotionPipelines.emplace(key, p);
        return p;
    }

    // Resolve a CKinematics child to its mesh + render mode (skinned leaves only).
    template <class TChild>
    static bool ResolveSkinnedLeaf(TChild* child, VK_Render_Mesh*& mesh, u16& rmode)
    {
        if (auto* st = dynamic_cast<vkSkeletonX_ST*>(child)) { mesh = &st->m_mesh; rmode = st->RenderMode; }
        else if (auto* pm = dynamic_cast<vkSkeletonX_PM*>(child)) { mesh = &pm->m_mesh; rmode = pm->RenderMode; }
        else return false;
        return mesh->p_rm_Vertices && mesh->p_rm_Indices && mesh->iCount != 0;
    }

    // nfs4: R4's per-leaf geometry LOD for progressive skeletons. Every skinned pass drew sliding window 0 (full
    // detail) at any range -- vkSkeletonX_PM::Load pins m_mesh there -- and on the Switch the 40-60 NPCs of the
    // Cordon camp, most of them 30-60 m away behind houses, cost ~11.7 ms of a 22 ms GPU frame (nx_diag mode 4).
    // R4 (OGSR r__dsgraph_build / r2_R_calculate / FSkinned.cpp):
    //   SSA = R / dist^2 (leaf sphere, world centre),
    //   lod = sqrt(clamp((SSA - GLOD_end) / (GLOD_start - GLOD_end), 0.01, 1)),
    //   GLOD_x = (ps_r__GLOD_ssa_x / 3)^2 / (W * H * (90 / fov)^2 * (EPS_S + ps_r__LOD)),
    //   lod_id = floor((1 - lod) * (count - 1) + 0.5).
    // A pure function of (leaf, xform, frame camera): colour, depth prepass, shadows and motion vectors pick the SAME
    // window in a frame, which the LEQUAL equality between them needs. The HUD (hands) always draws window 0.
    // r_skin_lod (default 1 = R4 behaviour on every platform; 0 = the old full detail everywhere).
    static bool SkinLodOn()
    {
        return ps_r_skin_lod != 0;
    }
    template <class TChild>
    static u32 SkinLeafWindowPM(TChild* child, const Fmatrix& xform, const vkSkeletonX_PM*& pmOut);
    // 07-10 / 09-10: the windows built for a leaf (vkSkeletonX_ST::AppendIndexLods; vkSkinLodWin for a PM leaf) by
    // distance: `lim` m to the first, twice that to each next; scaled by the fov so a scope keeps detail. The same pure
    // function of (leaf, xform, frame camera) as the R4 path: every pass picks the same window.
    static u32 SkinWindowByDistance(const Fvector& centre, const Fmatrix& xform, float lim, u32 count)
    {
        Fvector c;
        xform.transform_tiny(c, centre);
        const float fov = (RV().fFOV > 1.f) ? RV().fFOV : 67.f;
        const float d = RV().vCameraPosition.distance_to(c) * (fov / 67.f);
        u32 id = 0;
        for (; id + 1 < count && d > lim; lim *= 2.f)
            ++id;
        return id;
    }
    // What a leaf draws: index buffer, first index, index count. m_mesh.iBase already includes the first window's offset
    // (PM: pinned at load; ST: 0).
    struct SkinDraw
    {
        VkBuffer ib;
        u32 first;
        u32 count;
    };
    // 09-10: the r_skin_lod_bg windows (vkSkinLodWin) once live -- from the frame set in liveFrame; window 0 = the leaf's own
    // range. False = none (yet): the leaf's other windows. v306: the distance is scaled by the size of the whole model (its
    // radius over 1 m, a stalker's): a rifle in NPC hands (wpn_ak74: 26 leaves, ~100k triangles) is half a body's size on
    // screen and drops at half the distance, an artefact at 0.3 of it; a body, a mutant or a vehicle as before.
    static bool SkinBgWindow(const vkSkinLodWin* w, const CKinematics* K, const Fvector& centre, const Fmatrix& xform, SkinDraw& d)
    {
        if (ps_r_skin_lod_bg <= 0.f || !w || !w->live(RV().frameIndex))
            return false;
        const float size = K ? clampr(K->vis.sphere.R, .3f, 1.f) : 1.f;
        // 10-10 r_skin_lod_wpn (Switch 1): a weapon's own ladder -- 50% from 1 m, then 25 / 12 / 6 / 3% at 2 / 4 / 8 / 16 m.
        // A gun is far denser than a body of its size: by the body's ladder a winchester1300 (radius 1, a stalker's) drew all
        // its 111k triangles to 6 m, and its coarsest window was still 13k.
        const u32 id = (w->weapon && ps_r_skin_lod_wpn > 0.f) ? SkinWindowByDistance(centre, xform, ps_r_skin_lod_wpn, w->count)
                                                             : SkinWindowByDistance(centre, xform, ps_r_skin_lod_bg * size, w->count);
        if (id)
            d = {w->ib->GetHandle(), w->off[id], w->cnt[id]};
        return true;
    }
    template <class TChild>
    static SkinDraw SkinLeafDraw(TChild* child, const VK_Render_Mesh* mesh, const Fmatrix& xform, bool hud)
    {
        SkinDraw d{mesh->p_rm_Indices->GetHandle(), mesh->iBase, mesh->iCount};
        if (hud)
        {
            // 10-10 r_skin_hud_lod: the first-person hands/weapon at a fixed window (1 = 50%, 2 = 25%) -- at arm's length,
            // so no distance ladder. The A/B in hudwpn_v321: holstering the rifle took w.skinned 1.28 -> 0.12 units.
            if (ps_r_skin_hud_lod > 0)
            {
                const std::shared_ptr<vkSkinLodWin>* sp = nullptr;
                if (const vkSkeletonX_PM* pm = dynamic_cast<const vkSkeletonX_PM*>(child)) sp = &pm->skinLod;
                else if (const vkSkeletonX_ST* st = dynamic_cast<const vkSkeletonX_ST*>(child)) sp = &st->skinLod;
                const vkSkinLodWin* w = sp ? sp->get() : nullptr;
                if (w)
                    vkSkinLodWin_Kick(*sp); // 11-10: built on its first first-person draw (vk_Visual.cpp)
                if (w && w->live(RV().frameIndex) && w->count > 1)
                {
                    const u32 id = _min(u32(ps_r_skin_hud_lod), w->count - 1);
                    d = {w->ib->GetHandle(), w->off[id], w->cnt[id]};
                }
            }
            return d;
        }
        if (!SkinLodOn())
            return d;
        if (const vkSkeletonX_PM* pm = dynamic_cast<const vkSkeletonX_PM*>(child))
        {
            if (SkinBgWindow(pm->skinLod.get(), pm->Parent, pm->vis.sphere.P, xform, d))
                return d;
            const vkSkeletonX_PM* r4;
            const u32 id = SkinLeafWindowPM(child, xform, r4);
            if (r4)
                d = {d.ib, mesh->iBase - pm->sw_offsets[0] + pm->sw_offsets[id], pm->sw_counts[id]};
            return d;
        }
        if (const vkSkeletonX_ST* st = dynamic_cast<const vkSkeletonX_ST*>(child))
        {
            if (ps_r_skin_st_lod > 0.f && st->st_lod_count > 1)
            {
                const u32 id = SkinWindowByDistance(st->vis.sphere.P, xform, ps_r_skin_st_lod, st->st_lod_count);
                d = {d.ib, mesh->iBase - st->st_lod_off[0] + st->st_lod_off[id], st->st_lod_cnt[id]};
            }
            else
                SkinBgWindow(st->skinLod.get(), st->Parent, st->vis.sphere.P, xform, d);
        }
        return d;
    }
    template <class TChild>
    static u32 SkinLeafWindowPM(TChild* child, const Fmatrix& xform, const vkSkeletonX_PM*& pmOut)
    {
        pmOut = nullptr;
        const vkSkeletonX_PM* pm = dynamic_cast<const vkSkeletonX_PM*>(child);
        if (!pm || pm->sw_count < 2 || !pm->sw_offsets || !pm->sw_counts || pm->vis.sphere.R <= 0.f)
            return 0;
        pmOut = pm;
        Fvector c;
        xform.transform_tiny(c, pm->vis.sphere.P);
        const float distSQ = RV().vCameraPosition.distance_to_sqr(c) + EPS;
        const float ssa = pm->vis.sphere.R / distSQ;
        const float fov = (RV().fFOV > 1.f) ? RV().fFOV : 67.f;
        const float scr = float(_max(RV().dwWidth, 1u)) * float(_max(RV().dwHeight, 1u)) * _sqr(90.f / fov) * (EPS_S + ps_r__LOD);
        // nfs7: r_skin_lod_k scales the R4 thresholds; the Switch preset's 4 switches at HALF the R4 distance (SSA = R / d^2,
        // so thresholds x4 = distance x0.5): a figure 30 m away is a few dozen pixels on the handheld screen. 1 = R4.
        const float kLodDist = ps_r_skin_lod_k;
        const float gStart = kLodDist * _sqr(ps_r__GLOD_ssa_start / 3.f) / scr;
        const float gEnd = kLodDist * _sqr(ps_r__GLOD_ssa_end / 3.f) / scr;
        const float lod = _sqrt(clampr((ssa - gEnd) / (gStart - gEnd), 0.01f, 1.f));
        int id = iFloor((1.f - lod) * float(pm->sw_count - 1) + 0.5f);
        clamp(id, 0, int(pm->sw_count) - 1);
        return u32(id);
    }
    template <class TChild>
    static u32 SkinLeafCount(TChild* child, const VK_Render_Mesh* mesh, const Fmatrix& xform, bool hud)
    {
        return SkinLeafDraw(child, mesh, xform, hud).count;
    }

    // GLASS leaves of the world list (OGF "models\window": cabinet/vehicle panes and the
    // INVISIBLE BLOODSUCKER's predator body) blend without z-write. Drawn inline in
    // Pass_World, everything the frame draws after it -- trees, grass, LODs, the sky
    // (z == 1 pixels), water -- overwrote them wherever it lay BEHIND them: a stalking
    // bloodsucker vanished against the sky and the grass showed through his legs.
    // Same bug the static panes had (see Pass_WorldGlass); same cure -- they go to the
    // late "Glass" pass. Pass_Skinned raises the flag when it held any back.
    enum : u32 { kGlassInline = 0u, kGlassDefer = 1u, kGlassOnly = 2u };
    bool s_glassDeferred = false;

    // Draw one list of uploaded skeletons. Bones in the SSBO are already
    // world-space (pre-multiplied at upload), so viewProj is pushed as-is.
    // lastPipe/lastMatSet persist across lists (viewport changes between lists
    // don't disturb pipeline/descriptor bindings â€” both are dynamic / separate).
    // glassMode: kGlassInline = everything here (the HUD), kGlassDefer = skip the glass
    // leaves, kGlassOnly = only them, in the late Glass scope (pipeline variant 4).
    static void DrawSkinnedList(VkCommandBuffer cmd, const xr_vector<SkelUpload>& list,
                                const Fmatrix& viewProj, u32& nDraw,
                                VkPipeline& lastPipe, VkDescriptorSet& lastMatSet, u32& lastWin, VkDescriptorSet lightSet,
                                float hudMode, u32 glassMode = kGlassInline)
    {
        for (const SkelUpload& u : list)
        {
            for (auto* child : u.K->children)
            {
                VK_Render_Mesh* mesh = nullptr;
                u16 rmode = 0;
                if (!ResolveSkinnedLeaf(child, mesh, rmode)) continue;

                // Collimator/red-dot marks: additive unlit pipeline (R4
                // hud_reddotsight). Glass panes (kinematics furniture/doors,
                // OGF "glass" shader / glas\ texture): lit blended, no z-write.
                // Lightplanes leaves are never drawn — they only register their
                // synthesized beam (Pass_LightCones draws the real cone).
                const bool emissive = child->m_bEmissiveAdd;
                const bool glass    = child->m_bModelGlass;
                const bool litblend = child->m_bLitBlend;
                const bool lateGlass = glass && !emissive && !litblend;   // what the late Glass pass owns
                if (glassMode == kGlassOnly && !lateGlass) continue;
                if (glassMode == kGlassDefer && lateGlass) { s_glassDeferred = true; continue; }
                if (litblend)
                {
                    // Register the synthesized beam cone (Pass_LightCones draws
                    // it later this frame). Single-bone fans follow their bone —
                    // hidden bone (torch off: X-Ray zeroes it) = no beam, and a
                    // rotating searchlight carries its cone. HUD skipped: a
                    // camera-apex beam is milk (the real flashlight spot covers it).
                    if (hudMode < 0.5f && child->m_SynthBeams.count)
                    {
                        // bone visibility / transform come from the extract-time copy (see BeamBone)
                        bool    visible = true;
                        Fmatrix xf      = u.xform;
                        for (const BeamBone& bb : u.beams)
                            if (bb.child == (const void*)child) { visible = bb.visible; xf = bb.xf; break; }
                        if (visible) SynthCones::Submit(child, xf);
                        else         SynthCones::Revoke(child);   // torch off — kill the beam now
                    }
                    // The fake sheets themselves are never drawn — Pass_LightCones
                    // draws the REAL volumetric cone from the synthesized light.
                    continue;
                }
                const DrawSrc ds = ResolveDrawSrc(child, mesh, rmode, u.baseBone, u.boneCount);
                const BoneRoute br = RouteBones(ds.stride, ds.baseBone, ds.boneCount, s_vsUbo);
                VkPipeline pipe = GetPipeline(br.key, emissive ? 1u : (glass ? (glassMode == kGlassOnly ? 4u : 2u) : 0u), hudMode > 0.5f);
                if (pipe == VK_NULL_HANDLE) continue;

                if (pipe != lastPipe) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 2, 1, &lightSet, 0, nullptr);  // set2 = env lighting (per-frame)
                    lastPipe = pipe;
                    lastMatSet = VK_NULL_HANDLE;   // pipeline change disturbs set bindings â€” rebind material
                    lastWin = kUnbound;
                }
                if (br.win != lastWin) {   // set 0 = bones: once per pipeline, then once per UBO window
                    BindBones(cmd, s_layout, br.win);
                    lastWin = br.win;
                }

                // Diffuse texture (set 1) from the leaf's WorldMaterial; fall back to default white.
                WorldMaterial* mat = child->m_pWorldMaterial ? child->m_pWorldMaterial : WorldMaterialCache::GetDefault();
                VkDescriptorSet matSet = (mat && mat->set != VK_NULL_HANDLE) ? mat->set : VK_NULL_HANDLE;
                if (matSet == VK_NULL_HANDLE) continue;   // no texture descriptor â†’ can't sample, skip
                if (matSet != lastMatSet) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 1, 1, &matSet, 0, nullptr);
                    lastMatSet = matSet;
                }

                // RenderMode enum -> skinMode: RM_SINGLE(1)/RM_SKINNING_1B(2)->1, 2B(3)->2, 3B(4)->3, 4B(5)->4.
                // Bit 4 (16) = emissive-add flag for the fragment (unlit output);
                // bit 5 (32) = glass (skip the alpha test, cap the blend alpha);
                // the vertex shader masks them off before the skinning switch.
                u32 skinMode = ds.skinMode;
                if (emissive) skinMode |= 16u;
                if (glass)    skinMode |= 32u;
                SkinPush pc{ viewProj, skinMode, br.baseBone, ds.boneCount, hudMode, u.hemi };
                vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

                VkDeviceSize vbOff = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &ds.vb, &vbOff);
                const SkinDraw skd = SkinLeafDraw(child, mesh, u.xform, hudMode > 0.5f);
                vkCmdBindIndexBuffer(cmd, skd.ib, 0, mesh->iType);
                vkCmdDrawIndexed(cmd, skd.count, 1, skd.first, ds.firstVertex, 0);
                ++nDraw;
            }
        }
    }

    // HUD near-depth viewport (matches R4 rmNear = depth range [0, 0.02]): the HUD
    // weapon/hands occupy the front 2% of depth so they always win the depth test
    // against the world without clipping into it. Keeps the negative-height Y-flip.
    static void SetViewportDepth(VkCommandBuffer cmd, const VkExtent2D& ext, float minD, float maxD)
    {
        VkViewport vp{ 0.0f, (float)ext.height, (float)ext.width, -(float)ext.height, minD, maxD };
        vkCmdSetViewport(cmd, 0, 1, &vp);
    }
}  // anon namespace

// GLASS refraction, kinematics half: re-draw this frame's glass leaves (cabinet
// panes etc.) into the ALREADY-BEGUN heat-haze distortion pass with the variant-3
// pipeline (GPU skinning + procedural wobble). Called by Pass_Particles phase 3;
// SF().uploads (bones incl.) is still this frame's data there. strength rides the
// SkinPush hemi slot. HUD list excluded (no world positions, never refracts).
void Skinned_RenderGlassDistort(VkCommandBuffer cmd, const Fmatrix& viewProj, float strength)
{
    if (SF().uploads.empty() || s_set == VK_NULL_HANDLE) return;
    VkPipeline lastPipe = VK_NULL_HANDLE;
    for (const SkelUpload& u : SF().uploads)
    {
        for (auto* child : u.K->children)
        {
            VK_Render_Mesh* mesh = nullptr; u16 rmode = 0;
            if (!ResolveSkinnedLeaf(child, mesh, rmode)) continue;
            if (!child->m_bModelGlass) continue;
            const DrawSrc ds = ResolveDrawSrc(child, mesh, rmode, u.baseBone, u.boneCount);
            VkPipeline pipe = GetPipeline(ds.stride, 3u);
            if (pipe == VK_NULL_HANDLE) continue;
            if (pipe != lastPipe) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &s_set, 0, nullptr);   // set0 = bones
                lastPipe = pipe;
            }
            SkinPush pc{ viewProj, ds.skinMode, ds.baseBone, ds.boneCount, 0.0f, strength };
            vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            VkDeviceSize vbOff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &ds.vb, &vbOff);
            const SkinDraw skd = SkinLeafDraw(child, mesh, u.xform, false);
            vkCmdBindIndexBuffer(cmd, skd.ib, 0, mesh->iType);
            vkCmdDrawIndexed(cmd, skd.count, 1, skd.first, ds.firstVertex, 0);
        }
    }
}

// The world glass leaves Pass_Skinned held back this frame, drawn by Pass_WorldGlass
// inside its overlay scope (LOAD colour+depth, test, no write) after the whole opaque
// world, sky and water. Same leaves, same draws -- only later; no glass, no cost.
bool Skinned_HasLateGlass() { return s_glassDeferred; }

void Skinned_RenderGlassLate(VkCommandBuffer cmd, const Fmatrix& viewProj)
{
    if (!s_glassDeferred || s_failed) return;
    const VkDescriptorSet lightSet = EnvLight::GetCurrentSet();
    if (lightSet == VK_NULL_HANDLE) return;
    VkPipeline      lastPipe   = VK_NULL_HANDLE;
    VkDescriptorSet lastMatSet = VK_NULL_HANDLE;
    u32             lastWin    = kUnbound;
    u32             nDraw      = 0;
    DrawSkinnedList(cmd, SF().uploads, viewProj, nDraw, lastPipe, lastMatSet, lastWin, lightSet, 0.0f, kGlassOnly);
}

// ⭐ПРИБОР `[VK Bones/up]` (при `cpu_budget >= 2`). Фаза prepare показала
// `pre Skin/bones` 0.54 core-ms, а прибор `[Bones]` в SkeletonRigid.cpp насчитал
// в самой CalculateBones только 0.41 -- и то по ВСЕМ вызывающим, не только по
// рендеру. Значит часть цены в СОБСТВЕННОМ цикле этой функции, и её надо
// назвать, прежде чем что-то выносить: у четырёх кусков РАЗНЫЙ риск на воркере.
//   cast  -- dynamic_cast<CKinematics*> на визуал;
//   calc  -- CalculateBones (внутри: колбэки анимации и костей ⇒ ОПАСНО);
//   mul   -- bone*xform в отображённый SSBO: чистая математика, БЕЗ колбэков;
//   diag  -- поиск «ненимированных» визуалов.
namespace {
    u64 s_ubCast = 0, s_ubCalc = 0, s_ubMul = 0, s_ubDiag = 0, s_ubProbe = 0, s_ubCalcStatic = 0, s_ubPre = 0;
    u32 s_ubSkel = 0, s_ubBones = 0, s_ubFrames = 0, s_ubLastMs = 0, s_ubSkelStatic = 0;
    // ⚠ДВА РАЗНЫХ КРИТЕРИЯ, И ИХ ПУТАЛИ. `Static` -- «костных колбэков нет ВООБЩЕ»;
    // по нему печаталась строка «safe for r_bones_mt 2», хотя РЕЖИМ 2 пускает на
    // пул по БЕЛОМУ СПИСКУ (BoneCallbackIsMtSafe: IK ног, физоболочки, прицел).
    // То есть прибор мерил ДРУГОЙ, заведомо более узкий набор, и размер приза по
    // нему занижен. `Vetted` -- тот же предикат, что у режима 2.
    u64 s_ubCalcVetted = 0;
    u32 s_ubSkelVetted = 0;
    // Предпроход разложен надвое: ОТБОР (обход визуалов + сканирование костей
    // на пригодность) и САМ СЧЁТ (своя доля + непроверенные + раздача).
    // Без этого 0.19 core-ms под режимом 2 нечем объяснить.
    u64 s_ubCollect = 0, s_ubDispatch = 0;
    inline u64 UbCycles() { ULONG64 c = 0; QueryThreadCycleTime(GetCurrentThread(), &c); return u64(c); }
    void UbMaybeLog()
    {
        ++s_ubFrames;
        const u32 now = (u32)xrPlatform::TickCount64();
        if (!s_ubLastMs) { s_ubLastMs = now; return; }
        if (now - s_ubLastMs < 5000) return;
        const double cpm = CpuBudget::CyclesPerMs();
        const double f = double(s_ubFrames ? s_ubFrames : 1);
        Msg("[VK Bones/up] %.1f skeletons, %.0f bones/frame | prepass %.3f + cast %.3f + probe %.3f + calc %.3f + mul %.3f + diag %.3f"
            " = %.3f core-ms/frame",
            double(s_ubSkel) / f, double(s_ubBones) / f, double(s_ubPre) / cpm / f, double(s_ubCast) / cpm / f, double(s_ubProbe) / cpm / f,
            double(s_ubCalc) / cpm / f, double(s_ubMul) / cpm / f, double(s_ubDiag) / cpm / f,
            double(s_ubPre + s_ubCast + s_ubProbe + s_ubCalc + s_ubMul + s_ubDiag) / cpm / f);
        // ⭐РАЗМЕР ПРИЗА: у скольких скелетов НЕТ костных колбэков (то есть их
        // можно считать на пуле, не унося туда игровой код) и сколько они стоят.
        // 📏19-09: 45 из 116 скелетов, но всего 23% цены — дорогие как раз те,
        // у кого колбэки есть (актёр, NPC с физикой и прицелом).
        Msg("[VK Bones/up]   prepass split: collect (visual walk + bone vetting scan) %.3f + dispatch (own share, unvetted, join) %.3f",
            double(s_ubCollect) / cpm / f, double(s_ubDispatch) / cpm / f);
        Msg("[VK Bones/up]   VETTED (what r_bones_mt 2 ACTUALLY admits -- white list): %.1f skeletons, "
            "calc %.3f core-ms/frame (%.0f%% of calc)",
            double(s_ubSkelVetted) / f, double(s_ubCalcVetted) / cpm / f,
            s_ubCalc ? 100.0 * double(s_ubCalcVetted) / double(s_ubCalc) : 0.0);
        Msg("[VK Bones/up]   callback-free (NARROWER criterion, kept for reference): %.1f skeletons, "
            "calc %.3f core-ms/frame (%.0f%% of calc) | probe itself %.3f",
            double(s_ubSkelStatic) / f, double(s_ubCalcStatic) / cpm / f,
            s_ubCalc ? 100.0 * double(s_ubCalcStatic) / double(s_ubCalc) : 0.0, double(s_ubProbe) / cpm / f);
        s_ubCast = s_ubCalc = s_ubMul = s_ubDiag = s_ubProbe = s_ubCalcStatic = s_ubPre = 0;
        s_ubCalcVetted = 0; s_ubSkelVetted = 0;
        s_ubCollect = s_ubDispatch = 0;
        s_ubSkel = s_ubBones = s_ubFrames = s_ubSkelStatic = 0;
        s_ubLastMs = now;
    }
}

// r_bones_lod_dist (12-10-2026, Switch; "as GTA does"): a world skeleton farther than this from the camera keeps the
// pose it got less than 30 ms ago -- its bones are calculated every other frame (30 Hz), the GPU skins it with the
// matrices it already has. The blends still advance by the clock, only the in-between poses are not made. In a
// 12-bandit fight CalculateBones was ~4 % of the main thread. 0 = off. Never the HUD.
static bool BonesLodSkip(const DynVisual& d, CKinematics* K)
{
    if (ps_r_bones_lod_dist <= 0.f || !K->LastCalcTime())
        return false;
    if (Device.dwTimeGlobal - K->LastCalcTime() >= 30u)
        return false;
    return d.xform.c.distance_to_sqr(RV().vCameraPosition) > _sqr(ps_r_bones_lod_dist);
}

void Skinned_UploadBones()
{
    if (!Init()) return;
    if (SF().uploadFrame == RV().frameIndex) return;   // once per frame
    // Two-slot pipeline: bones are EXTRACT work (game bone callbacks, CalculateBones, the bone ring). The record-side calls
    // scattered through the passes are idempotent fallbacks; on the render thread they must never do the work -- a frame whose
    // PREPARE did not run just has no skinned data (and draws none).
    if (RT::tl_role == RT::Render)
    {
        static bool s_said = false;
        if (!s_said) { s_said = true; Msg("![VK Skinned] Skinned_UploadBones reached on the render thread without PREPARE -- skipped (frame draws no skinned data)"); }
        return;
    }
    SF().uploadFrame = RV().frameIndex;
    // PM windows the skin worker finished: live from the NEXT frame, so a pass of this one recorded before this point
    // (a record-side fallback call) cannot pick another window than the rest of the frame
    vkSkinLodWin_Publish(RV().frameIndex + 1);

    const bool ubMeasure = (CpuBudget::enabled >= 2);
    u64 ubMark = ubMeasure ? UbCycles() : 0;
    SF().uploads.clear();
    SF().uploadsHud.clear();
    SF().uploadsShadow.clear();

    // Roll the bone-slot records: this frame's map becomes "previous" for the MV
    // pass (its SSBO regions are still live), and we rebuild "current" below.
    // Two slots: "last frame's" cur* is the OTHER slot's, which the next extract overwrites while this frame's record
    // still reads it -- so this slot takes a copy and owns it.
    {
        const SkinFrame& prev = s_skinSlots.at(RT::Slot() + 1);
        SF().prevBoneMap = prev.curBoneMap;
        SF().prevBoneMapHud = prev.curBoneMapHud;
    }
    SF().curBoneMap.clear();
    SF().curBoneMapHud.clear();

    // -----------------------------------------------------------------------
    // ⭐ПРЕДПРОХОД: КОСТИ ВСЕХ СКЕЛЕТОВ ПАРАЛЛЕЛЬНО (`r_bones_mt`, дефолт 0).
    //
    // Почему это вообще стоит делать: 📏`calc` = 0.44-0.48 core-ms/кадр на ~113
    // скелетах — третья статья главного потока после планировщика и записи
    // World. Почему МОЖНО: `CKinematics::CalculateBones` держит per-object
    // `UCalc_Mutex` (то есть разные объекты независимы ПО ЗАМЫСЛУ), а её хвост
    // прямо гейтит `Update_Callback` на `TTAPI->on_pool_thread()` — движок уже
    // ожидал вызовов с пула. Единственная нелокальная запись, глобальный
    // `::Random` в ветке виде-бокса, убрана (см. SkeletonRigid.cpp).
    //
    // ⚠⚠ЧЕГО ЭТО НЕ ДОКАЗЫВАЕТ. Тело зовёт КОСТНЫЕ КОЛБЭКИ (физика, прицел,
    // руль) и колбэки конца анимации — игровой код, который читать глазами
    // бесполезно ([[parallel-record-blocked-by-shared-mutable-state]]: там
    // аудит по файлу пасса сломался двумя уровнями ниже). Поэтому дефолт 0,
    // а инструмент проверки — прогон с `-vk_validation` и краш, не чтение.
    // ⚠И «картинка не поехала» здесь тоже ничего не значит: гонка поз
    // проявится позой на кадр, а шум сцены больше.
    //
    // Порядок: кусок 0 остаётся ГЛАВНОМУ потоку (он всё равно ждёт), остальные
    // уходят в пул. Дубли между списками безвредны: второй вызов упирается в
    // тот же мьютекс и выходит по `dwTimeGlobal == UCalc_Time`.
    // ⭐⭐РЕЖИМЫ: 1 = ВСЕ скелеты на пул, 2 = ТОЛЬКО БЕЗ КОСТНЫХ КОЛБЭКОВ.
    // 📏Почему появился режим 2. Под режимом 1 счётчик показал **~170 костных
    // колбэков в кадр на чужих потоках** против 0.5-2.9 в базе -- то есть выигрыш
    // 0.30 core-ms куплен тем, что физические оболочки, прицел и руль поехали
    // исполняться на восьми потоках сразу. Это ровно тот класс правки, который
    // падает не сегодня. Режим 2 оставляет скелеты с колбэками главному потоку:
    // у остальных `Bone_Calculate` -- чистая математика (блендинг + умножение
    // матриц), и никакого xrGame в ней нет.
    if (ps_r_bones_mt && TTAPI)
    {
        static xr_vector<CKinematics*>      s_mtSkel;    // на пул
        static xr_vector<CKinematics*>      s_mtSerial;  // с колбэками -- главному потоку
        static xr_vector<std::future<void>> s_mtJobs;
        s_mtSkel.clear();
        s_mtSerial.clear();
        const bool cbSafeOnly = (ps_r_bones_mt >= 2);
        auto collect = [cbSafeOnly](const xr_vector<DynVisual>& list, bool lod) {
            for (const DynVisual& d : list) {
                if (!d.vis) continue;
                CKinematics* K = dynamic_cast<CKinematics*>(d.vis);
                if (!K || K->children.empty()) continue;
                if (lod && BonesLodSkip(d, K)) continue;   // r_bones_lod_dist: keeps last frame's pose
                if (cbSafeOnly) {
                    // ⭐БЕЛЫЙ СПИСОК, а не «колбэков нет вовсе» (bone.h).
                    // Первая версия режима 2 отдавала пулу только скелеты
                    // БЕЗ колбэков и дала всего −0.035: таких 45 из 116, но
                    // это 23% цены. Перепись `[BoneCb]` показала, что три
                    // семейства, дающие 179 срабатываний из 179, все читающие
                    // — значит отсеивать надо не «есть колбэк», а «есть
                    // НЕПРОВЕРЕННЫЙ колбэк». Ранний выход по первому такому.
                    bool unvetted = false;
                    const u16 nb = K->LL_BoneCount();
                    for (u16 i = 0; i < nb && !unvetted; ++i)
                        if (!BoneCallbackIsMtSafe(K->LL_GetBoneInstance(i).callback())) unvetted = true;
                    (unvetted ? s_mtSerial : s_mtSkel).push_back(K);
                } else {
                    s_mtSkel.push_back(K);
                }
            }
        };
        const u64 mtA = ubMeasure ? UbCycles() : 0;
        collect(DynamicVisuals(), true);
        collect(HudVisuals(), false);
        collect(ShadowOnlyVisuals(), true);
        const u64 mtB = ubMeasure ? UbCycles() : 0;
        if (ubMeasure) s_ubCollect += mtB - mtA;

        const u32 n = (u32)s_mtSkel.size();
        if (n > 1 && ps_r_bones_mt >= 2)
        {
            // ⭐07-10-2026: a worker PINNED to core 1 (the render thread's; it runs ~50%
            // there in a firefight) claims skeletons one at a time with the pool and the
            // main thread. The chunked fork-join below handed its chunks to the TTAPI
            // pool, whose threads libnx starts on core 0 -- the main thread's own core,
            // so on the Switch the "parallel" chunks mostly took turns with it.
            static std::atomic<u32> s_next{0};
            static PinnedWorker* s_bonesWorker = new PinnedWorker(1); // leaked: no join at exit
            auto drain = [] {
                const u32 cnt = (u32)s_mtSkel.size();
                for (u32 i = s_next.fetch_add(1, std::memory_order_relaxed); i < cnt; i = s_next.fetch_add(1, std::memory_order_relaxed))
                    s_mtSkel[i]->CalculateBones(TRUE);
            };
            s_next.store(0, std::memory_order_relaxed);
            s_bonesWorker->SetPrio(ps_r_core1_prio);   // the main thread waits for it: not below the render thread on core 1
            s_bonesWorker->Run(drain);
            // the main thread: the skeletons that must stay here first, then whatever is left
            for (CKinematics* K : s_mtSerial)
                K->CalculateBones(TRUE);
            drain();
            s_bonesWorker->Wait();
        }
        else if (n > 1)
        {
            // Куски, а не задача на скелет: 113 фьючерсов стоили бы больше, чем
            // считают. Потолок 8 -- дальше выигрыш съедает раздача.
            const u32 lanes = _max(1u, _min(TTAPI->get_num_threads(), 8u));
            const u32 chunk = (n + lanes - 1) / lanes;
            s_mtJobs.clear();
            for (u32 a = chunk; a < n; a += chunk)
            {
                const u32 b = _min(n, a + chunk);
                s_mtJobs.push_back(TTAPI->submit([a, b]() {
                    for (u32 i = a; i < b; ++i) s_mtSkel[i]->CalculateBones(TRUE);
                }));
            }
            // Пока пул считает свои куски, главный поток делает СВОЮ долю и тех,
            // кого нельзя отдавать. Ждём только после этого.
            for (u32 i = 0, e = _min(chunk, n); i < e; ++i)
                s_mtSkel[i]->CalculateBones(TRUE);
            for (CKinematics* K : s_mtSerial)
                K->CalculateBones(TRUE);
            for (auto& f : s_mtJobs)
                if (f.valid()) f.wait();
            s_mtJobs.clear();
        }
        else
        {
            for (CKinematics* K : s_mtSerial)
                K->CalculateBones(TRUE);
        }
        if (ubMeasure) s_ubDispatch += UbCycles() - mtB;
    }

    // ⚠Предпроход закрывается СВОЕЙ графой. Без этой строки он попадал в `cast`
    // (там первый замер после начала функции), и прибор показывал «dynamic_cast
    // подорожал в десять раз» вместо «появилась новая фаза». Прибор, который
    // приписывает работу не тому, кто её сделал, хуже отсутствующего.
    if (ubMeasure) { const u64 n = UbCycles(); s_ubPre += n - ubMark; ubMark = n; }

    // Pick THIS frame's bone-SSBO region by the fence-guarded in-flight slot, so the
    // CPU never writes matrices a still-in-flight frame's GPU may be reading.
    const u32 slot  = RV().ringSlot;
    u32 cursor      = slot * kMaxBones;
    const u32 limit = cursor + kMaxBones;

    // Slot 0 of this frame's region is reserved for IDENTITY: pre-skinned leaves
    // are drawn as 1-weight geometry pointing at it, so the consumer shaders'
    // `S * pos` / `mat3(S) * nrm` pass the already-skinned world values through
    // untouched (see Skinned_PreSkin). Costs one matrix per frame.
    SF().identityBone = cursor;
    s_boneMapped[cursor].identity();
    ++cursor;

    auto uploadList = [&](const xr_vector<DynVisual>& list, xr_vector<SkelUpload>& out, bool lod) {
        for (const DynVisual& d : list)
        {
            if (!d.vis) continue;
            CKinematics* K = dynamic_cast<CKinematics*>(d.vis);  // CKinematics : FHierrarhyVisual : vkRender_Visual
            if (!K || K->children.empty()) continue;

            // bForceExact=TRUE: recompute bones every frame (matches R4's render
            // path, r__dsgraph_build.cpp pV->CalculateBones(TRUE)). With FALSE the
            // shared CKinematics::CalculateBones takes its "slow update" early-out
            // (RV().dwTimeGlobal < UCalc_Time + UCalc_Interval) and leaves bones
            // STALE for UCalc_Interval ms â†’ visibly jerky animation. The
            // dwTimeGlobal==UCalc_Time guard inside still prevents double-advance.
            if (ubMeasure) { const u64 n = UbCycles(); s_ubCast += n - ubMark; ubMark = n; }

            // ⭐ЕСТЬ ЛИ ВООБЩЕ ЧЕМУ МЕНЯТЬСЯ В ЭТОМ СКЕЛЕТЕ.
            // Костные матрицы МОДЕЛЬНЫЕ: мировая позиция приезжает ниже через
            // d.xform, поэтому едущий ящик их не меняет. Меняют их ровно две
            // вещи -- активный бленд и костный колбэк (физика/прицел/руль).
            // Если нет ни того, ни другого, Bone_Calculate каждый кадр выдаёт
            // ПОБИТОВО ТО ЖЕ. Пока только СЧИТАЕМ, сколько таких и чего они
            // стоят: сначала цена приза, потом правка.
            bool staticPose = false, vettedPose = false;
            if (ubMeasure)
            {
                u32 blends_now = 0;
                if (auto* ka = dynamic_cast<CKinematicsAnimated*>(K))
                    for (u32 p = 0; p < 4; ++p) blends_now += ka->LL_PartBlendsCount(p);
                // ОДИН проход по костям на ОБА критерия: раннего выхода нет, зато
                // и второго прохода тоже. Цена пробы и так 0.063 core-ms.
                bool anyCb = false, unvetted = false;
                const u16 nb = K->LL_BoneCount();
                for (u16 i = 0; i < nb; ++i)
                {
                    const BoneCallback cb = K->LL_GetBoneInstance(i).callback();
                    if (!cb) continue;
                    anyCb = true;
                    if (!BoneCallbackIsMtSafe(cb)) { unvetted = true; break; }
                }
                staticPose = !anyCb;    // узкий критерий, для сравнения
                vettedPose = !unvetted; // ⭐ТОТ ЖЕ предикат, что у режима 2
                const u64 n = UbCycles(); s_ubProbe += n - ubMark; ubMark = n;
            }

            if (!(lod && BonesLodSkip(d, K)))   // r_bones_lod_dist: the pose of less than 30 ms ago stays
                K->CalculateBones(TRUE);
            if (ubMeasure)
            {
                const u64 n = UbCycles();
                s_ubCalc += n - ubMark;
                if (staticPose) { s_ubCalcStatic += n - ubMark; ++s_ubSkelStatic; }
                if (vettedPose) { s_ubCalcVetted += n - ubMark; ++s_ubSkelVetted; }
                ubMark = n; ++s_ubSkel;
            }
            const u16 bc = K->LL_BoneCount();
            if (bc == 0) continue;
            if (cursor + bc > limit) break;   // this frame's SSBO region full

            // Store boneÂ·xform (modelâ†’WORLD): S*pos in the shaders is then world-
            // space, the main pass pushes plain viewProj, the shadow pass plain
            // lightVP, and normals get the object rotation they previously missed.
            const u32 base = cursor;
            for (u16 i = 0; i < bc; ++i)
                s_boneMapped[cursor + i].mul(d.xform, K->LL_GetTransform_R(i));
            if (ubMeasure) { const u64 n = UbCycles(); s_ubMul += n - ubMark; ubMark = n; s_ubBones += bc; }

            // diag: sample skeletons round-robin -- if a
            // bone lands kilometres from its object xform (never-animated bones,
            // garbage matrices), the mesh "draws" but collapses off-screen.
            // blends: are the PlayCycle'd loops actually attached to this visual?
            {
                static u32 s_boneDiag = 0;
#ifdef __SWITCH__
                if (false)   // v104: 650 log lines a run on the console's SD card; the PC keeps the probe
#else
                if (++s_boneDiag % 501 == 1)
#endif
                {
                    const Fmatrix& b0 = s_boneMapped[base];
                    const Fmatrix& bl = s_boneMapped[base + bc - 1];
                    u32 nBlends = 0;
                    if (auto* ka = dynamic_cast<CKinematicsAnimated*>(K))
                        for (u32 p = 0; p < 4; ++p)
                            nBlends += ka->LL_PartBlendsCount(p);
                    VisMask vm = K->LL_GetBonesVisible();
                    const Fvector mT0 = K->LL_GetBoneInstance(0).mTransform.c;
                    Msg("[VK Skinned] up-diag: bones=%u blends=%u vis=0x%llx/0x%llx mT0=(%.2f,%.2f,%.2f) xform=(%.1f,%.1f,%.1f) bone0=(%.1f,%.1f,%.1f) boneN=(%.1f,%.1f,%.1f)",
                        bc, nBlends, vm._visimask.flags, vm._visimask_ex.flags, VPUSH(mT0), VPUSH(d.xform.c), VPUSH(b0.c), VPUSH(bl.c));
                }

                // ⭐⭐⭐WHICH SKINNED VISUAL PUTS VERTICES IN THE SKY.
                //
                // Reported from play twice and hunted from the game side for
                // three builds without success: a body standing in a bind pose
                // under a fan of grey spikes tens of metres tall. The game-side
                // sweep never even saw the object, which rules out puppets,
                // abandoned player bodies and the first-person body (all three
                // tested and cleared) -- so it is a visual the game does not own.
                //
                // ⚠A bind pose ALONE cannot make spikes: identity skinning
                // matrices simply draw the mesh where it was authored, which is
                // the T-pose we see. Vertices leave for infinity when the shader
                // reads bone matrices OUTSIDE this skeleton's slice, i.e. when a
                // child's declared RenderMode disagrees with the vertex layout
                // the shader is told to assume (skinMode below). So name the mode
                // per child, for every unanimated skeleton, once each.
                //
                // ⭐Costs nothing in the normal case: an animated skeleton has
                // blends and never reaches this branch.
                // ⚠Строго говоря, РАССУЖДЕНИЕ выше неточно: `dynamic_cast` и
                // четыре LL_PartBlendsCount стоят ДО условия, то есть платятся
                // на каждый скелет каждый кадр. Но ✅ИЗМЕРЕНО 19-09 (A/B за
                // временным кваром, Кордон, ~40 скелетов): 0.400-0.440 против
                // 0.419-0.452 core-ms на всю `Skinned_UploadBones` — разницы
                // нет, ВЫВОД комментария верен. Квар убран, диагностика
                // остаётся включённой. Полкадровой миллисекунды здесь тратит
                // CalculateBones, а не это.
                {
                    u32 blends_now = 0;
                    if (auto* ka = dynamic_cast<CKinematicsAnimated*>(K))
                        for (u32 p = 0; p < 4; ++p)
                            blends_now += ka->LL_PartBlendsCount(p);

                    // ⚠⚠ONCE-PER-VISUAL WAS THE WRONG CADENCE, AND IT COST A
                    // WRONG DIAGNOSIS.
                    //
                    // The first cut printed each visual once and never again. Its
                    // xform was therefore a FIRST-SIGHT SNAPSHOT, and reading
                    // those stale coordinates next to live creature positions
                    // "showed" the phantom bodies riding on the NPCs. They were
                    // not: when the positions were finally compared at the same
                    // instant, the phantoms sat 80 m away, and a fix built on the
                    // false correlation had to be reverted (see
                    // CAttachmentOwner::renderable_Render).
                    //
                    // ⭐So: re-announce the same visual every two seconds. A
                    // diagnostic whose value goes stale must say WHEN it is
                    // speaking, or it invents relationships that are not there.
                    //
                    // Bone names come along because they identify the MODEL CLASS
                    // without any name string being available in a release build:
                    // `bip01_*` is a body, `wpn_*` a weapon, anything else a prop.
                    if (0 == blends_now)
                    {
                        struct Seen { CKinematics* K; u32 ms; };
                        static xr_vector<Seen> s_seen;
                        const u32 now_ms = RV().dwTimeGlobal;
                        auto it = std::find_if(s_seen.begin(), s_seen.end(), [K](const Seen& s) { return s.K == K; });
                        const bool due = (it == s_seen.end()) || (now_ms - it->ms >= 2000);
                        if (due && s_seen.size() < 64)
                        {
                            if (it == s_seen.end())
                                s_seen.push_back({K, now_ms});
                            else
                                it->ms = now_ms;

                            u32 kids = 0;
                            string512 modes{};
                            for (auto* child : K->children)
                            {
                                VK_Render_Mesh* mesh = nullptr;
                                u16 rmode = 0;
                                if (!ResolveSkinnedLeaf(child, mesh, rmode))
                                    continue;
                                ++kids;
                                const u32 skinMode = (rmode <= 2u) ? 1u : (u32(rmode) - 1u);
                                string32 one;
                                xr_sprintf(one, "%s%u->%u", kids > 1 ? "," : "", u32(rmode), skinMode);
                                xr_strcat(modes, one);
                            }
                            // first and last bone name: enough to tell a body from a prop
                            LPCSTR b_first = (bc > 0) ? K->LL_BoneName(0) : "?";
                            LPCSTR b_last = (bc > 0) ? K->LL_BoneName(u16(bc - 1)) : "?";
                            Msg("![VK Skinned] UNANIMATED visual @%u: bones=%u children=%u bone0='%s' boneN='%s' rmode->skinMode[%s] xform=(%.1f,%.1f,%.1f)", now_ms, bc, kids,
                                b_first, b_last, modes, VPUSH(d.xform.c));
                        }
                    }
                }
            }
            if (ubMeasure) { const u64 n = UbCycles(); s_ubDiag += n - ubMark; ubMark = n; }
            cursor += bc;
            {
                SkelUpload su{ K, d.xform, base, bc, d.hemi, d.noShadow };
                su.sphere = K->vis.sphere;
                if (&out != &SF().uploadsHud)   // beams are not registered for the HUD (a camera-apex beam is milk)
                    for (auto* child : K->children)
                    {
                        if (!child->m_bLitBlend || !child->m_SynthBeams.count) continue;
                        u32 boneId = u32(-1);
                        if (auto* st = dynamic_cast<vkSkeletonX_ST*>(child)) {
                            if (st->RenderMode == vkSkeletonX_ST::RM_SINGLE) boneId = st->RMS_boneid;
                        } else if (auto* pm = dynamic_cast<vkSkeletonX_PM*>(child)) {
                            if (pm->RenderMode == vkSkeletonX_PM::RM_SINGLE) boneId = pm->RMS_boneid;
                        }
                        BeamBone bb{ child, true, d.xform };
                        if (boneId != u32(-1)) {
                            bb.visible = !!K->LL_GetBoneVisible((u16)boneId);
                            if (bb.visible) bb.xf.mul_43(d.xform, K->LL_GetBoneInstance((u16)boneId).mRenderTransform);
                        }
                        su.beams.push_back(bb);
                    }
                out.push_back(std::move(su));
            }
        }
    };
    SF().characters.clear();
    for (const DynVisual& d : DynamicVisuals())
        if (CKinematics* K = d.vis ? dynamic_cast<CKinematics*>(d.vis) : nullptr)
        {
            // A skeleton of glass/marks only casts nothing (Skinned_RenderShadow's rule) -- the invisible
            // bloodsucker (models\window body + selflight eyes) kept its capsule shadow (r_shadow_lite) here.
            bool opaque = false;
            for (auto* child : K->children)
                if (!child->m_bModelGlass && !child->m_bEmissiveAdd && !child->m_bLitBlend) { opaque = true; break; }
            SF().characters.push_back({ d.vis, d.xform, d.vis->vis.box, d.noShadow || !opaque });
        }
    uploadList(DynamicVisuals(),   SF().uploads, true);
    uploadList(HudVisuals(),       SF().uploadsHud, false);
    uploadList(ShadowOnlyVisuals(), SF().uploadsShadow, true);

    // Record this frame's world-skeleton slots so NEXT frame's MV pass can find the
    // previous pose (HUD excluded â€” it never enters the MV pass).
    for (const SkelUpload& u : SF().uploads)
        SF().curBoneMap[u.K] = { u.baseBone, (u32)u.boneCount };
    // HUD weapon now has its own MV overlay (Skinned_RenderMotionHud), so track
    // its prev poses too — last frame's SSBO region stays live (FRAMES_IN_FLIGHT>=2).
    for (const SkelUpload& u : SF().uploadsHud)
        SF().curBoneMapHud[u.K] = { u.baseBone, (u32)u.boneCount };

    if (ubMeasure) UbMaybeLog();
}

// ===========================================================================
//  COMPUTE PRE-SKINNING  (r_preskin)
// ===========================================================================
namespace {

static bool PreSkinInit()
{
    if (s_psInited) return !s_psFailed;
    s_psInited = true;

    VkShaderModule cs = g_ShaderManager->Load("preskin.comp.spv");
    if (cs == VK_NULL_HANDLE) {
        Msg("![VK PreSkin] preskin.comp.spv missing - falling back to per-pass skinning");
        s_psFailed = true; return false;
    }

    // set 0: binding 0 = the SAME bone SSBO the graphics pipelines read,
    //        binding 1 = the shared output pool.
    //        binding 2 = the previous-pose pool (r_mv_prepass).
    constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (!VK::MakeDescriptorSets({ kSSBO, kSSBO, kSSBO }, 1, s_psSetL, s_psPool, &s_psSet,
                                VK_SHADER_STAGE_COMPUTE_BIT, "PreSkin")) {
        s_psFailed = true; return false;
    }

    // gpuOnly=TRUE is mandatory: a STORAGE buffer without it gets
    // HOST_ACCESS_SEQUENTIAL_WRITE, so VMA puts it in the BAR heap or plain
    // system RAM and every read rides PCIe (see vk_buffer.h's warning and the
    // VSM saga). Nothing on the CPU ever touches this pool.
    // DEVICE_ADDRESS + AS_BUILD_INPUT: the dynamic BLAS (vk_rt_dyn) is built
    // straight out of this pool — the world-space vertices the raster draws are
    // exactly what a ray should hit, with no second copy.
    s_psBuf.Create(VkDeviceSize(kPsBytesSlot) * kFramesInFlight,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, /*gpuOnly*/ true);
    if (!s_psBuf.IsValid()) { Msg("![VK PreSkin] pool alloc failed"); s_psFailed = true; return false; }
    Prof::NameBuffer(s_psBuf.GetHandle(), "PreSkinPool");
    s_psPoolAddr = s_psBuf.GetDeviceAddress();

    s_psPrevBuf.Create(VkDeviceSize(kPsPrevBytesSlot) * kFramesInFlight,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, /*gpuOnly*/ true);
    if (!s_psPrevBuf.IsValid()) { Msg("![VK PreSkin] prev-pose pool alloc failed"); s_psFailed = true; return false; }
    Prof::NameBuffer(s_psPrevBuf.GetHandle(), "PreSkinPrevPool");

    VK::DescriptorWriter(s_psSet)
        .StorageBuffer(0, s_boneSSBO.GetHandle())
        .StorageBuffer(1, s_psBuf.GetHandle())
        .StorageBuffer(2, s_psPrevBuf.GetHandle())
        .Flush();

    s_psLayout = VK::MakePipelineLayout({ s_psSetL }, sizeof(PreSkinPush));
    if (s_psLayout == VK_NULL_HANDLE) { s_psFailed = true; return false; }

    s_psPipe = VK::CreateComputePipeline(cs, s_psLayout, "PreSkin");
    if (s_psPipe == VK_NULL_HANDLE) { s_psFailed = true; return false; }

    Msg("[VK PreSkin] init OK (pool %u MB/slot x%u = %u verts/slot, stride %u)",
        kPsBytesSlot >> 20, kFramesInFlight, kPsVertsSlot, kPsStride);
    return true;
}

}  // anon namespace

// ⭐РЕШЕНИЕ КАДРА ПО ПРЕ-СКИННИНГУ. Главный поток, фаза prepare, ДО первой
// записанной команды (VK::RegisterPrepare в CRender::Render). Строит всё, что
// потом ЧИТАЮТ чужие пассы, и не пишет ни одной команды:
//   SF().uploads/SF().curBoneMap (через Skinned_UploadBones) · SF().psMap · SF().psRtLeaves
//   · SF().psJobs (список диспатчей, который проиграет тело пасса).
// Идемпотентна по RV().frameIndex: тело пасса зовёт её же, поэтому кадр без
// фазы prepare (редактор, чужой порядок) работает ровно как раньше.
// Leaves that don't fit keep the classic per-pass skinning: SF().psMap simply has
// no entry for them.
void Skinned_PreSkinPrepare()
{
    if (!Init()) return;
    if (SF().psFrame == RV().frameIndex) return;   // once per frame
    if (RT::tl_role == RT::Render) return;   // extract work, see Skinned_UploadBones
    SF().psFrame = RV().frameIndex;

    Skinned_UploadBones();   // CPU bones + the identity slot this pass points at

    SF().psMap.clear();
    SF().psRtLeaves.clear();
    SF().psJobs.clear();
    s_psLeaves = s_psVerts = s_psOverflow = s_psSkipped1W = s_psMaxBones = 0;
    if (!ps_r_preskin) return;                             // A/B: fall back to per-pass skinning
    if (SF().uploads.empty() && SF().uploadsHud.empty() && SF().uploadsShadow.empty()) return;
    if (!PreSkinInit()) return;

    VK_CPU_PROBE("cpu:PreSkin");

    const u32 slot   = RV().ringSlot;
    u32       cursor = slot * kPsVertsSlot;
    const u32 limit  = cursor + kPsVertsSlot;

    // r_rt_dyn: the dynamic BLAS is built from this pool, so when a ray may hit
    // the skeletons EVERY leaf goes through here — the 1-weight skip below is a
    // raster economy, and a door or a crate has to be in the BVH to occlude.
    const bool rtWant = RTDyn::Wanted();
    auto skinList = [&](const xr_vector<SkelUpload>& list,
                        const std::unordered_map<CKinematics*, std::pair<u32, u32>>& prevMap,
                        bool forRt) {
        for (const SkelUpload& u : list)
        {
            if (u.boneCount > s_psMaxBones) s_psMaxBones = u.boneCount;
            // Where this skeleton's bones sat LAST frame — the previous pose the
            // shader also skins into the parallel pool. A skeleton not seen last
            // frame (or whose bone count changed) falls back to this frame's base:
            // previous == current == zero motion, the right answer for a spawn.
            u32 prevBase = u.baseBone;
            {
                auto pit = prevMap.find(u.K);
                if (pit != prevMap.end() && pit->second.second == (u32)u.boneCount)
                    prevBase = pit->second.first;
            }
            // A ray never reaches a skeleton past the RTAO cutoff (+ the ray length),
            // so the far ones stay out of the BLAS — the first Cordon build took
            // every skeleton in the frustum: 414 k triangles, 1.1 ms a frame.
            const bool rtNear = rtWant && forRt &&
                u.xform.c.distance_to(RV().vCameraPosition) <= ps_r_rtao_dist + ps_r_rtao_radius + 4.f;
            for (auto* child : u.K->children)
            {
                VK_Render_Mesh* mesh = nullptr;
                u16 rmode = 0;
                if (!ResolveSkinnedLeaf(child, mesh, rmode)) continue;
                // Lightplanes fans are never drawn (Pass_LightCones renders the
                // synthesized cone instead) - skinning them would be pure waste.
                if (child->m_bLitBlend) continue;

                // 1-WEIGHT GEOMETRY CAN NEVER WIN HERE — skip it.
                // RM_SINGLE / RM_SKINNING_1B both map to skinMode 1, whose classic
                // vertex shader is already a SINGLE matrix fetch (`S = bones[idx]`).
                // Pre-skinning would replace that with... a single matrix fetch of
                // the identity, and still pay the compute dispatch + pool write.
                // Measured on l01_escape 2026-07-23: 46% of the 583 visible leaves
                // were 1-3 bone props (doors, lamps, physics crates, dropped guns)
                // and they dominated the pool, which is why the first A/B came out
                // net negative. Only a real multi-bone blend (2W/3W/4W — NPCs) has
                // anything to save.
                const u32 skinMode = (rmode <= 2u) ? 1u : (u32(rmode) - 1u);
                const bool rtLeaf = rtNear && mesh->iAddr != 0 && mesh->iCount >= 3;
                if (skinMode < 2u && !rtLeaf) { ++s_psSkipped1W; continue; }

                const u32 vc = mesh->vCount;
                if (!vc || !mesh->vAddr) continue;         // not addressable -> classic path
                if (cursor + vc > limit) { ++s_psOverflow; continue; }   // pool full -> classic path

                if (rtLeaf) {
                    PreSkinRtLeaf L{};
                    L.poolFirst  = cursor;
                    L.vertCount  = vc;
                    L.ibAddr     = mesh->iAddr + VkDeviceSize(mesh->iBase) * (mesh->iType == VK_INDEX_TYPE_UINT16 ? 2u : 4u);
                    L.indexCount = mesh->iCount;
                    L.iType      = mesh->iType;
                    SF().psRtLeaves.push_back(L);
                }

                PreSkinPush pc{};
                pc.src         = mesh->vAddr + VkDeviceSize(mesh->vBase) * mesh->vStride;
                pc.vertCount   = vc;
                pc.srcStrideDW = mesh->vStride / 4u;
                pc.dstFirst    = cursor;
                pc.skinMode    = skinMode;
                pc.baseBone    = u.baseBone;
                pc.boneCount   = (u32)u.boneCount;
                pc.prevBase    = prevBase;
                SF().psJobs.push_back({ pc, (vc + 63u) / 64u });

                SF().psMap[(const void*)child] = cursor;
                cursor += vc;
                ++s_psLeaves;
            }
        }
    };
    skinList(SF().uploads,       SF().prevBoneMap,    true);
    skinList(SF().uploadsHud,    SF().prevBoneMapHud, false);   // never a ray target: the HUD would occlude the whole near field
    skinList(SF().uploadsShadow, SF().prevBoneMap,    false);   // shadow-only casters skin here too, or they draw in bind pose

    s_psVerts = cursor - slot * kPsVertsSlot;
    if (s_psVerts > s_psPeak) s_psPeak = s_psVerts;

    if (!s_psLeaves) {
        // Nothing multi-bone on screen. Worth saying out loud: it means every
        // skinned leaf this frame was 1-weight prop geometry, so any measurement
        // taken here says nothing about pre-skinning on an NPC crowd.
        static u32 s_idle = 0;
        if (++s_idle % 601 == 1)
            Msg("[VK PreSkin] idle: 0 multi-bone leaves (skipped1W=%u, maxBones=%u) - prop-only scene",
                s_psSkipped1W, s_psMaxBones);
        SF().psJobs.clear();   // ничего не диспатчим -- и барьер в теле пасса тоже не нужен
        return;
    }
}

// Skin every visible leaf ONCE into the shared pool. Registered as the "PreSkin"
// pass so it runs before Pass_SunShadow (the earliest consumer) and outside any
// dynamic-rendering scope. ⭐ТОЛЬКО ЗАПИСЬ: весь разбор скелетов и построение
// SF().psMap уже случились в Skinned_PreSkinPrepare выше.
void Skinned_PreSkin(VkCommandBuffer cmd)
{
    // (The leaf walk and the job list were built by the PREPARE phase "PreSkin/leaves" -- extract side.)
    if (SF().psJobs.empty()) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_psPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_psLayout, 0, 1, &s_psSet, 0, nullptr);
    for (const PreSkinJob& j : SF().psJobs)
    {
        vkCmdPushConstants(cmd, s_psLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(j.pc), &j.pc);
        vkCmdDispatch(cmd, j.groups, 1, 1);
    }

    // The pool is read as vertex attributes by every consumer from here on.
    VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);

    static bool s_first = false;
    if (!s_first) { s_first = true;
        Msg("[VK PreSkin] first dispatch: leaves=%u verts=%u (%u%% of slot) skeletons=%zu+%zu",
            s_psLeaves, s_psVerts, (s_psVerts * 100u) / kPsVertsSlot, SF().uploads.size(), SF().uploadsHud.size()); }
    static u32 s_pulse = 0;
    if (++s_pulse % 601 == 1)
        Msg("[VK PreSkin] leaves=%u verts=%u peak=%u/%u overflow=%u | skipped1W=%u maxBones=%u",
            s_psLeaves, s_psVerts, s_psPeak, kPsVertsSlot, s_psOverflow, s_psSkipped1W, s_psMaxBones);
}

// The dynamic BLAS inputs (vk_rt_dyn). The list is THIS frame's — the frame
// check is the same one ResolveDrawSrc makes: a stale list would point a build
// at another in-flight slot's vertices.
VkDeviceAddress Skinned_PreSkinPoolAddress() { return s_psPoolAddr; }
const xr_vector<PreSkinRtLeaf>& Skinned_PreSkinRtLeaves()
{
    static const xr_vector<PreSkinRtLeaf> s_none;
    return (SF().psFrame == RV().frameIndex) ? SF().psRtLeaves : s_none;
}

void Skinned_CollectFeet(xr_vector<Fvector>& out, u32 maxFeet)
{
    out.clear();
    if (!s_inited || !s_boneMapped) return;
    // STALKER human bipeds: feet = bip01_l_foot / bip01_r_foot (fallback l_foot/r_foot).
    // s_boneMapped[baseBone+id] is already model->WORLD, so .c is the world foot pos.
    for (const SkelUpload& u : SF().uploads) {
        if (!u.K) continue;
        u16 lf = u.K->LL_BoneID("bip01_l_foot"); if (lf == BI_NONE) lf = u.K->LL_BoneID("l_foot");
        u16 rf = u.K->LL_BoneID("bip01_r_foot"); if (rf == BI_NONE) rf = u.K->LL_BoneID("r_foot");
        // ONLY real foot bones (bipeds). NO object-root fallback: the stamp is XZ-only
        // (ignores height), so stamping a skeleton-less object's root made FLYING items
        // (thrown bolt/grenade) leave a ground trail in mid-air.
        if (lf != BI_NONE && lf < u.boneCount) { out.push_back(s_boneMapped[u.baseBone + lf].c); if (out.size() >= maxFeet) return; }
        if (rf != BI_NONE && rf < u.boneCount) { out.push_back(s_boneMapped[u.baseBone + rf].c); if (out.size() >= maxFeet) return; }
    }
}

// DISCRETE footstep detection: a walking foot alternates SWING (fast in world space)
// and STANCE (planted, ~zero world velocity). Track each foot across frames and emit
// ONE event on the transition into stance away from its previous plant â€” separate
// boot prints per step instead of the dragged trench that per-frame stamping made.
namespace {
    struct FootTrack {
        Fvector lastPos{};      // world pos last frame
        Fvector lastPlant{};    // where this foot last printed
        int     settle  = -1;   // frames until the pending print is emitted (-1 = none)
        bool    planted = false;
        bool    init    = false;
        u32     frame   = 0;    // last seen (prune)
    };
    xr_map<const void*, FootTrack> s_footTracks;
}

void Skinned_CollectFootsteps(xr_vector<Footstep>& out, u32 maxSteps)
{
    out.clear();
    if (!s_inited || !s_boneMapped) return;
    const float dt = (RV().fTimeDelta > 1e-4f) ? RV().fTimeDelta : 1e-4f;
    constexpr float kPlantSpeed  = 0.45f;   // stance foot is ~stationary in world space (m/s)
    constexpr float kLiftSpeed   = 0.90f;   // swing foot moves fast -> re-arm for the next plant
    constexpr float kReplantDist = 0.28f;   // min XZ travel from the previous print (no idle pile-up)

    for (const SkelUpload& u : SF().uploads) {
        if (!u.K) continue;
        u16 lf = u.K->LL_BoneID("bip01_l_foot"); if (lf == BI_NONE) lf = u.K->LL_BoneID("l_foot");
        u16 rf = u.K->LL_BoneID("bip01_r_foot"); if (rf == BI_NONE) rf = u.K->LL_BoneID("r_foot");
        u16 lt = u.K->LL_BoneID("bip01_l_toe0"); if (lt == BI_NONE) lt = u.K->LL_BoneID("l_toe");
        u16 rt = u.K->LL_BoneID("bip01_r_toe0"); if (rt == BI_NONE) rt = u.K->LL_BoneID("r_toe");
        Fvector fwd = u.xform.k; fwd.y = 0.f;   // body facing (fallback boot orientation)
        if (fwd.square_magnitude() > 1e-4f) fwd.normalize(); else fwd.set(0.f, 0.f, 1.f);
        const u16 feet[2] = { lf, rf };
        const u16 toes[2] = { lt, rt };
        for (int s = 0; s < 2; ++s) {
            const u16 id = feet[s];
            if (id == BI_NONE || id >= u.boneCount) continue;
            const Fvector pos = s_boneMapped[u.baseBone + id].c;
            FootTrack& t = s_footTracks[(const void*)(((uintptr_t)u.K << 1) | (uintptr_t)s)];
            t.frame = RV().frameIndex;
            if (!t.init) { t.init = true; t.planted = true; t.lastPos = pos; t.lastPlant = pos; continue; }
            const float dx = pos.x - t.lastPos.x, dz = pos.z - t.lastPos.z;
            const float speed = sqrtf(dx * dx + dz * dz) / dt;
            if (speed > kLiftSpeed) {
                t.planted = false; t.settle = -1;        // swinging -> armed, cancel any pending print
            } else if (speed < kPlantSpeed) {
                if (!t.planted) {
                    // Heel-strike detected — but the foot is still decelerating/rotating
                    // down for a few cm. Don't print yet: let it fully SETTLE and stamp
                    // at the rested position a few frames later.
                    const float px = pos.x - t.lastPlant.x, pz = pos.z - t.lastPlant.z;
                    if (px * px + pz * pz > kReplantDist * kReplantDist) t.settle = 3;
                    t.planted = true;
                } else if (t.settle > 0) {
                    --t.settle;
                } else if (t.settle == 0) {
                    t.settle = -1;
                    if (out.size() < maxSteps) {
                        // The foot bone pivot is the ANKLE — above the boot rear, off the
                        // sole centre. Centre the print on the actual BOOT via the toe
                        // bone: sole centre ≈ ankle↔toe midpoint, ankle→toe = this foot's
                        // TRUE facing (feet splay while walking, not the body's k).
                        Fvector p = pos; float fx = fwd.x, fz = fwd.z;
                        const u16 tid = toes[s];
                        const bool haveToe = (tid != BI_NONE && tid < u.boneCount);
                        if (haveToe) {
                            const Fvector toe = s_boneMapped[u.baseBone + tid].c;
                            const float tx = toe.x - pos.x, tz = toe.z - pos.z;
                            const float tl = sqrtf(tx * tx + tz * tz);
                            if (tl > 0.04f) { fx = tx / tl; fz = tz / tl; }
                            p.x = 0.5f * (pos.x + toe.x); p.z = 0.5f * (pos.z + toe.z);
                        } else {
                            p.x += fx * 0.06f; p.z += fz * 0.06f;   // no toe bone: nudge ahead of the ankle
                        }
                        static int s_plantLog = 0;   // one-shot diag: is the toe bone actually found?
                        if (s_plantLog < 6) { ++s_plantLog;
                            Msg("[VK Steps] plant %s toe=%s ankle=(%.2f,%.2f) print=(%.2f,%.2f) dir=(%.2f,%.2f)",
                                s ? "R" : "L", haveToe ? "yes" : "NO", pos.x, pos.z, p.x, p.z, fx, fz);
                        }
                        out.push_back({ p, fx, fz });
                        t.lastPlant = pos;
                    }
                }
            }
            t.lastPos = pos;
        }
    }
    // Prune stale skeletons (despawn / level change) so the map doesn't grow forever.
    if (s_footTracks.size() > 160) {
        for (auto it = s_footTracks.begin(); it != s_footTracks.end(); )
            it = (RV().frameIndex - it->second.frame > 600) ? s_footTracks.erase(it) : ++it;
    }
}

// World PROPS / items (skeleton-less skinned visuals: thrown/dropped bolts, grenades,
// debris). Returns their world root. The caller gates these by ground proximity (so a
// FLYING item doesn't stamp) and prints them SHALLOW (an item sinks less than a boot).
void Skinned_CollectProps(xr_vector<Fvector>& out, u32 maxProps)
{
    out.clear();
    if (!s_inited || !s_boneMapped) return;
    for (const SkelUpload& u : SF().uploads) {
        if (!u.K) continue;
        u16 lf = u.K->LL_BoneID("bip01_l_foot"); if (lf == BI_NONE) lf = u.K->LL_BoneID("l_foot");
        u16 rf = u.K->LL_BoneID("bip01_r_foot"); if (rf == BI_NONE) rf = u.K->LL_BoneID("r_foot");
        if (lf != BI_NONE || rf != BI_NONE) continue;   // has feet -> biped, handled by CollectFeet
        out.push_back(u.xform.c);                        // object root (world)
        if (out.size() >= maxProps) return;
    }
}

// Same filter uploadList() uses to decide what this path owns. Kept next to it so the
// rigid sun-caster pass (vk_pass_shadow.cpp) can ask instead of repeating the test —
// change the filter and both follow.
const xr_vector<CharacterBounds>& Skinned_CharacterBounds() { return SF().characters; }

bool Skinned_HandlesVisual(IRenderVisual* v)
{
    if (!v)
        return false;
    CKinematics* K = dynamic_cast<CKinematics*>(v);
    return K && !K->children.empty();
}

void Skinned_RenderShadow(VkCommandBuffer cmd, const Fmatrix& lightVP,
                          const Fvector* cullPos, float cullRange, const CFrustum* frustum)
{
    if (!s_inited || s_failed || s_shadowVS == VK_NULL_HANDLE) return;
    // HUD never casts. SF().uploadsShadow does: it is the actor's own model, collected
    // by Render_SMAP precisely because the colour pass will never draw it.
    if (SF().uploads.empty() && SF().uploadsShadow.empty()) return;

    VkPipeline lastPipe = VK_NULL_HANDLE;
    u32 lastWin = kUnbound;   // set 0: bound per pipeline, then per bone window (r_skin_ubo)
    u32 nDraw = 0;

    // A no-shadow skeleton only stands down when its stand-in is actually there:
    // with g_actor_shadow off nothing is collected for SMAP, and the first-person
    // legs are the only silhouette you have — casting an armless shadow beats
    // casting none.
    const bool standIn = !SF().uploadsShadow.empty();

    auto drawList = [&](const xr_vector<SkelUpload>& list) {
    for (const SkelUpload& u : list)
    {
        if (u.noShadow && standIn) continue;
        // Cull: sun ortho box by default, light sphere for spot/point maps.
        const Fsphere& bs = u.sphere;
        if (bs.R > 0.f) {
            Fvector c; u.xform.transform_tiny(c, bs.P);
            if (cullPos) {
                const float rr = cullRange + bs.R;
                if (cullPos->distance_to_sqr(c) > rr * rr) continue;
                // Per-face frustum (point cube): skip the ~4 of 6 faces the NPC
                // isn't in — the sphere test alone drew it into all six.
                if (frustum && !frustum->testSphere_dirty(c, bs.R)) continue;
            } else if (!ShadowMap::SphereVisible(c, bs.R)) continue;
        }

        for (auto* child : u.K->children)
        {
            VK_Render_Mesh* mesh = nullptr;
            u16 rmode = 0;
            if (!ResolveSkinnedLeaf(child, mesh, rmode)) continue;
            if (child->m_bEmissiveAdd || child->m_bModelGlass || child->m_bLitBlend) continue;   // marks/glass don't cast shadows

            const DrawSrc ds = ResolveDrawSrc(child, mesh, rmode, u.baseBone, u.boneCount);
            const BoneRoute br = RouteBones(ds.stride, ds.baseBone, ds.boneCount, s_shadowVSUbo);
            VkPipeline pipe = GetShadowPipeline(br.key);
            if (pipe == VK_NULL_HANDLE) continue;

            if (pipe != lastPipe) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                lastPipe = pipe;
                lastWin = kUnbound;
            }
            if (br.win != lastWin) {
                BindBones(cmd, s_layout, br.win);
                lastWin = br.win;
            }

            SkinPush pc{ lightVP, ds.skinMode, br.baseBone, ds.boneCount, 0.0f, 1.0f };
            vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

            VkDeviceSize vbOff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &ds.vb, &vbOff);
            const SkinDraw skd = SkinLeafDraw(child, mesh, u.xform, false);
            vkCmdBindIndexBuffer(cmd, skd.ib, 0, mesh->iType);
            vkCmdDrawIndexed(cmd, skd.count, 1, skd.first, ds.firstVertex, 0);
            ++nDraw;
        }
    }
    };
    drawList(SF().uploads);
    drawList(SF().uploadsShadow);

    static bool s_diag = false;
    if (!s_diag && nDraw) { s_diag = true; Msg("[VK Skinned] first shadow render: skeletons=%zu(+%zu smap) draws=%u",
                                              SF().uploads.size(), SF().uploadsShadow.size(), nDraw); }
}

// NPCs into the scene depth PREPASS (camera VP): they become GTAO occluders â€”
// contact darkening on the ground under characters, like R4 where skinned
// geometry is in the gbuffer â€” and the world color pass gets early-Z behind
// them. Alpha-tested (a < 0.25, same as skinned.frag) so hair/strap cutouts
// don't punch holes into the early-Z'd background. HUD never enters (SF().uploads
// only). Caller owns render begin/end and viewport (Pass_World prepass).
// Shared draw loop for the near-camera skinned casters (depth prepass + normal
// G-buffer differ only by which pipeline they bind, picked via `getPipe`).
// `twin` = the pass's r_skin_ubo vertex shader (VK_NULL_HANDLE = SSBO only).
static void RenderSkinnedCasters(VkCommandBuffer cmd, const Fmatrix& viewProj, VkPipeline (*getPipe)(u32), VkShaderModule twin,
                                 bool hud = false)
{
    // AO is short-range â€” only NPCs near the camera matter as occluders.
    const float kPrepassRange = ps_r_skin_prepass_dist;

    VkPipeline      lastPipe   = VK_NULL_HANDLE;
    VkDescriptorSet lastMatSet = VK_NULL_HANDLE;
    u32             lastWin    = kUnbound;   // set 0: bound per pipeline, then per bone window (r_skin_ubo)

    // hud: the first-person list (r_hud_prepass) -- no range cut, and the leaf LOD windows the HUD colour pass picks.
    for (const SkelUpload& u : (hud ? SF().uploadsHud : SF().uploads))
    {
        const Fsphere& bs = u.sphere;
        if (!hud && bs.R > 0.f) {
            Fvector c; u.xform.transform_tiny(c, bs.P);
            const float rr = kPrepassRange + bs.R;
            if (RV().vCameraPosition.distance_to_sqr(c) > rr * rr) continue;
        }

        for (auto* child : u.K->children)
        {
            VK_Render_Mesh* mesh = nullptr;
            u16 rmode = 0;
            if (!ResolveSkinnedLeaf(child, mesh, rmode)) continue;
            if (child->m_bEmissiveAdd || child->m_bModelGlass || child->m_bLitBlend) continue;   // marks/glass: blended, no depth

            const DrawSrc ds = ResolveDrawSrc(child, mesh, rmode, u.baseBone, u.boneCount);
            const BoneRoute br = RouteBones(ds.stride, ds.baseBone, ds.boneCount, twin);
            VkPipeline pipe = getPipe(br.key);
            if (pipe == VK_NULL_HANDLE) continue;

            if (pipe != lastPipe) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                lastPipe = pipe;
                lastWin = kUnbound;
                lastMatSet = VK_NULL_HANDLE;
            }
            if (br.win != lastWin) {
                BindBones(cmd, s_layout, br.win);
                lastWin = br.win;
            }

            // Diffuse (set 1) for the alpha test â€” default white (a=1) never discards.
            WorldMaterial* mat = child->m_pWorldMaterial ? child->m_pWorldMaterial : WorldMaterialCache::GetDefault();
            VkDescriptorSet matSet = (mat && mat->set != VK_NULL_HANDLE) ? mat->set : VK_NULL_HANDLE;
            if (matSet == VK_NULL_HANDLE) continue;
            if (matSet != lastMatSet) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 1, 1, &matSet, 0, nullptr);
                lastMatSet = matSet;
            }

            SkinPush pc{ viewProj, ds.skinMode, br.baseBone, ds.boneCount, 0.0f, 1.0f };
            vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

            VkDeviceSize vbOff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &ds.vb, &vbOff);
            const SkinDraw skd = SkinLeafDraw(child, mesh, u.xform, hud);
            vkCmdBindIndexBuffer(cmd, skd.ib, 0, mesh->iType);
            vkCmdDrawIndexed(cmd, skd.count, 1, skd.first, ds.firstVertex, 0);
        }
    }
}

// r_mv_prepass: the NPC half of the prepass — lay the depth AND state the motion in
// ONE raster. Mirrors RenderSkinnedCasters exactly in WHICH leaves it draws (the same
// 60 m occluder range, the same marks/glass skips), because this replaces that walk:
// any leaf it dropped would lose its prepass depth, not just its motion vector.
// Returns false when the MV pipelines are unavailable, so the caller falls back to
// the plain (motion-attachment-declared, mask-0) prepass instead of drawing nothing.
static bool RenderSkinnedMotionPrepass(VkCommandBuffer cmd)
{
    // BOTH vertex shaders, not either: a scene mixes pre-skinned leaves (multi-bone
    // NPCs) with classic ones (1-weight props, pool overflow) and this walk REPLACES
    // the depth prepass for all of them. Missing one and skipping those leaves would
    // cost them their prepass DEPTH — GTAO occlusion and early-Z, not just a motion
    // vector — so if either is absent the whole path stands down and the plain
    // prepass runs instead.
    if (s_mvLayout == VK_NULL_HANDLE || s_mvFS == VK_NULL_HANDLE) return false;
    if (s_mvVS == VK_NULL_HANDLE || s_mvPreVS == VK_NULL_HANDLE)  return false;

    const float kPrepassRange = ps_r_skin_prepass_dist;   // the SAME radius, deliberately: this half replaces that walk

    float jx = 0.0f, jy = 0.0f;
    VK::MotionVec::FrameJitter(jx, jy);
    const Fmatrix& curVP  = VK::MotionVec::CurVP();    // UNJITTERED — jitter-free MV
    const Fmatrix& prevVP = VK::MotionVec::PrevVP();

    VkPipeline      lastPipe   = VK_NULL_HANDLE;
    VkDescriptorSet lastMatSet = VK_NULL_HANDLE;
    bool            boundBones = false;
    u32             nDraw = 0, nFallback = 0;

    for (const SkelUpload& u : SF().uploads)
    {
        const Fsphere& bs = u.sphere;
        if (bs.R > 0.f) {
            Fvector c; u.xform.transform_tiny(c, bs.P);
            const float rr = kPrepassRange + bs.R;
            if (RV().vCameraPosition.distance_to_sqr(c) > rr * rr) continue;
        }

        // Previous pose for the CLASSIC (not pre-skinned) leaves; the pre-skinned ones
        // get theirs as a vertex stream preskin.comp already wrote.
        u32 prevBase = u.baseBone;
        {
            auto it = SF().prevBoneMap.find(u.K);
            if (it != SF().prevBoneMap.end() && it->second.second == (u32)u.boneCount)
                prevBase = it->second.first;
        }

        for (auto* child : u.K->children)
        {
            VK_Render_Mesh* mesh = nullptr;
            u16 rmode = 0;
            if (!ResolveSkinnedLeaf(child, mesh, rmode)) continue;
            if (child->m_bEmissiveAdd || child->m_bModelGlass || child->m_bLitBlend) continue;   // marks/glass: blended, no depth

            const DrawSrc ds = ResolveDrawSrc(child, mesh, rmode, u.baseBone, u.boneCount);
            const bool preskinned = (ds.vb == s_psBuf.GetHandle());
            VkPipeline pipe = GetPrepassMotionPipeline(ds.stride, preskinned);
            if (pipe == VK_NULL_HANDLE) { ++nFallback; continue; }

            if (pipe != lastPipe) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                lastPipe = pipe; boundBones = false; lastMatSet = VK_NULL_HANDLE;
            }
            if (!boundBones) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mvLayout, 0, 1, &s_set, 0, nullptr);
                boundBones = true;
            }

            // Diffuse (set 1) for the alpha test — default white (a=1) never discards.
            WorldMaterial* mat = child->m_pWorldMaterial ? child->m_pWorldMaterial : WorldMaterialCache::GetDefault();
            VkDescriptorSet matSet = (mat && mat->set != VK_NULL_HANDLE) ? mat->set : VK_NULL_HANDLE;
            if (matSet == VK_NULL_HANDLE) continue;
            if (matSet != lastMatSet) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mvLayout, 1, 1, &matSet, 0, nullptr);
                lastMatSet = matSet;
            }

            MVSkinPush pc{ curVP, prevVP, ds.skinMode, ds.baseBone, prevBase, ds.boneCount, jx, jy };
            vkCmdPushConstants(cmd, s_mvLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

            // Pre-skinned: two streams, ONE vertexOffset. The pools are written in the
            // same order from the same cursor, so ds.firstVertex indexes both.
            VkBuffer     vbs[2] = { ds.vb, s_psPrevBuf.GetHandle() };
            VkDeviceSize offs[2] = { 0, 0 };
            vkCmdBindVertexBuffers(cmd, 0, preskinned ? 2u : 1u, vbs, offs);
            const SkinDraw skd = SkinLeafDraw(child, mesh, u.xform, false);
            vkCmdBindIndexBuffer(cmd, skd.ib, 0, mesh->iType);
            vkCmdDrawIndexed(cmd, skd.count, 1, skd.first, ds.firstVertex, 0);
            ++nDraw;
        }
    }

    static bool s_diag = false;
    if (!s_diag && (nDraw || nFallback)) {
        s_diag = true;
        // nFallback > 0 is a PIPELINE-CREATION failure, and those leaves just lost
        // their prepass depth (not only their motion) — say it as a warning.
        Msg("%s[VK Skinned] first prepass MV render: draws=%u, leaves with no pipeline=%u",
            nFallback ? "!" : "", nDraw, nFallback);
    }
    return true;
}

float Skinned_PrepassRange() { return ps_r_skin_prepass_dist; }

void Skinned_RenderDepthPrepass(VkCommandBuffer cmd, const Fmatrix& viewProj, bool mvTarget)
{
    if (!Init()) return;
    if (s_prepassVS == VK_NULL_HANDLE || s_prepassFS == VK_NULL_HANDLE) return;
    if (SF().uploads.empty()) return;
    // r_mv_prepass: draw the motion-writing twin instead — same leaves, same depth,
    // plus each pixel's true motion. If it can't run (shaders missing), fall through
    // to the plain caster built with the SAME attachment list (mask 0), because the
    // scope's colour-attachment count is not ours to change here.
    if (mvTarget && RenderSkinnedMotionPrepass(cmd)) return;
    RenderSkinnedCasters(cmd, viewProj, mvTarget ? &GetPrepassPipelineMV : &GetPrepassPipeline, s_prepassVSUbo);
}

// 10-09 r_hud_prepass: the first-person hands/weapon into the prepass depth, at the HUD's own near range [0, 0.02] and
// HUD-FOV projection -- the same depth the HUD colour pass writes later (same skinning math, LEQUAL passes). The world
// under the hands then fails early-Z instead of being shaded and painted over (they cover a corner of every frame).
// Caller owns the render scope (Pass_World prepass, no motion attachment); the viewport range is restored.
void Skinned_RenderHudDepthPrepass(VkCommandBuffer cmd, const VkExtent2D& ext)
{
    if (!ps_r_hud_prepass) return;
    if (!Init()) return;
    if (s_prepassVS == VK_NULL_HANDLE || s_prepassFS == VK_NULL_HANDLE) return;
    if (SF().uploadsHud.empty()) return;
    SetViewportDepth(cmd, ext, 0.0f, 0.02f);
    RenderSkinnedCasters(cmd, RV().mFullTransform_hud, &GetPrepassPipeline, s_prepassVSUbo, true);
    SetViewportDepth(cmd, ext, 0.0f, 1.0f);
}

// NPC normal G-buffer: same near-camera casters, writing world-space normals
// into the SSAO normal RT (depth-tested against the prepass depth, no write).
// Caller owns the render begin/end + viewport (Pass_World, after the prepass).
void Skinned_RenderNormalPrepass(VkCommandBuffer cmd, const Fmatrix& viewProj)
{
    if (!Init()) return;
    if (s_normalVS == VK_NULL_HANDLE || s_normalFS == VK_NULL_HANDLE) return;
    if (SF().uploads.empty()) return;
    RenderSkinnedCasters(cmd, viewProj, &GetNormalPipeline, s_normalVSUbo);
}

// NPC MOTION VECTORS (MV Phase 2a): re-draw the world skinned leaves into the
// motion target, skinning the CURRENT and PREVIOUS pose so each pixel carries its
// true screen motion (camera + animation + body travel). Depth-tested against the
// complete scene depth (LEQUAL, no write) â†’ only visible NPC pixels overwrite the
// camera/static field. Caller owns render begin/end + the negative-height viewport
// (MotionVec::ExecuteDynamic). HUD excluded. prevVP = last frame's view-proj.
void Skinned_RenderMotion(VkCommandBuffer cmd, const Fmatrix& curVP, const Fmatrix& prevVP,
                          float jitterNdcX, float jitterNdcY, float minRange)
{
    if (!Init()) return;
    if (s_mvVS == VK_NULL_HANDLE || s_mvFS == VK_NULL_HANDLE || s_mvLayout == VK_NULL_HANDLE) return;
    if (SF().uploads.empty()) return;

    VkPipeline lastPipe = VK_NULL_HANDLE;
    VkDescriptorSet lastMatSet = VK_NULL_HANDLE;
    bool boundBones = false;
    u32 nDraw = 0;

    for (const SkelUpload& u : SF().uploads)
    {
        // r_mv_prepass: skip what the prepass already wrote. The test is the exact
        // COMPLEMENT of RenderSkinnedCasters' keep-test (same radius, same sphere,
        // same slack) — an inconsistency here would either double-draw a skeleton or,
        // worse, leave one with neither half's motion.
        if (minRange > 0.0f) {
            const Fsphere& bs = u.sphere;
            if (bs.R > 0.f) {
                Fvector c; u.xform.transform_tiny(c, bs.P);
                const float rr = minRange + bs.R;
                if (RV().vCameraPosition.distance_to_sqr(c) <= rr * rr) continue;
            } else {
                continue;   // no bound → the prepass kept it unconditionally
            }
        }

        // Previous pose: same skeleton last frame. Its SSBO region is still live
        // (FRAMES_IN_FLIGHTâ‰¥2). New skeletons (not seen last frame, or whose bone
        // count changed) fall back to the current base â†’ camera-only motion.
        u32 prevBase = u.baseBone;
        auto it = SF().prevBoneMap.find(u.K);
        if (it != SF().prevBoneMap.end() && it->second.second == (u32)u.boneCount)
            prevBase = it->second.first;

        for (auto* child : u.K->children)
        {
            VK_Render_Mesh* mesh = nullptr;
            u16 rmode = 0;
            if (!ResolveSkinnedLeaf(child, mesh, rmode)) continue;
            if (child->m_bEmissiveAdd || child->m_bModelGlass || child->m_bLitBlend) continue;   // marks/glass: no depth, skip

            VkPipeline pipe = GetMotionPipeline(mesh->vStride);
            if (pipe == VK_NULL_HANDLE) continue;
            if (pipe != lastPipe) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                lastPipe = pipe; boundBones = false; lastMatSet = VK_NULL_HANDLE;
            }
            if (!boundBones) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mvLayout, 0, 1, &s_set, 0, nullptr);
                boundBones = true;
            }

            // Diffuse (set 1) for the alpha-test â€” default white (a=1) never discards.
            WorldMaterial* mat = child->m_pWorldMaterial ? child->m_pWorldMaterial : WorldMaterialCache::GetDefault();
            VkDescriptorSet matSet = (mat && mat->set != VK_NULL_HANDLE) ? mat->set : VK_NULL_HANDLE;
            if (matSet == VK_NULL_HANDLE) continue;
            if (matSet != lastMatSet) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mvLayout, 1, 1, &matSet, 0, nullptr);
                lastMatSet = matSet;
            }

            const u32 skinMode = (rmode <= 2u) ? 1u : (u32(rmode) - 1u);
            MVSkinPush pc{ curVP, prevVP, skinMode, u.baseBone, prevBase, (u32)u.boneCount, jitterNdcX, jitterNdcY };
            vkCmdPushConstants(cmd, s_mvLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

            VkBuffer vb = mesh->p_rm_Vertices->GetHandle();
            VkDeviceSize vbOff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOff);
            const SkinDraw skd = SkinLeafDraw(child, mesh, u.xform, false);
            vkCmdBindIndexBuffer(cmd, skd.ib, 0, mesh->iType);
            vkCmdDrawIndexed(cmd, skd.count, 1, skd.first, (s32)mesh->vBase, 0);
            ++nDraw;
        }
    }

    static bool s_diag = false;
    if (!s_diag && nDraw) { s_diag = true; Msg("[VK Skinned] first MV render: skeletons=%zu draws=%u", SF().uploads.size(), nDraw); }
}

// First-person HUD weapon MV overlay. Same mechanism as Skinned_RenderMotion, but
// for the HUD list and with the HUD-FOV projection (curHudVP/prevHudVP) at the
// near-depth viewport [0,0.02] the forward HUD pass used — so the weapon's clip
// depth bit-matches the HUD depth already in the buffer and LEQUAL passes. Without
// this the viewmodel (the closest, fastest-moving thing on screen) carries the
// fullscreen pass's bogus camera-reprojection MV and would ghost under DLSS/TAA.
// Caller (MotionVec::ExecuteDynamic) owns render begin/end; we restore the range.
void Skinned_RenderMotionHud(VkCommandBuffer cmd, const VkExtent2D& ext,
                             const Fmatrix& curHudVP, const Fmatrix& prevHudVP,
                             float jitterNdcX, float jitterNdcY)
{
    if (!Init()) return;
    if (s_mvVS == VK_NULL_HANDLE || s_mvFS == VK_NULL_HANDLE || s_mvLayout == VK_NULL_HANDLE) return;
    if (SF().uploadsHud.empty()) return;

    SetViewportDepth(cmd, ext, 0.0f, 0.02f);   // match the forward HUD near-depth range

    VkPipeline      lastPipe   = VK_NULL_HANDLE;
    VkDescriptorSet lastMatSet = VK_NULL_HANDLE;
    bool            boundBones = false;
    u32             nDraw      = 0;

    for (const SkelUpload& u : SF().uploadsHud)
    {
        // Previous pose: same HUD skeleton last frame (SSBO region still live).
        u32 prevBase = u.baseBone;
        auto it = SF().prevBoneMapHud.find(u.K);
        if (it != SF().prevBoneMapHud.end() && it->second.second == (u32)u.boneCount)
            prevBase = it->second.first;

        for (auto* child : u.K->children)
        {
            VK_Render_Mesh* mesh = nullptr;
            u16 rmode = 0;
            if (!ResolveSkinnedLeaf(child, mesh, rmode)) continue;
            if (child->m_bEmissiveAdd || child->m_bModelGlass || child->m_bLitBlend) continue;   // marks/glass: skip

            VkPipeline pipe = GetMotionPipeline(mesh->vStride);
            if (pipe == VK_NULL_HANDLE) continue;
            if (pipe != lastPipe) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                lastPipe = pipe; boundBones = false; lastMatSet = VK_NULL_HANDLE;
            }
            if (!boundBones) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mvLayout, 0, 1, &s_set, 0, nullptr);
                boundBones = true;
            }

            WorldMaterial* mat = child->m_pWorldMaterial ? child->m_pWorldMaterial : WorldMaterialCache::GetDefault();
            VkDescriptorSet matSet = (mat && mat->set != VK_NULL_HANDLE) ? mat->set : VK_NULL_HANDLE;
            if (matSet == VK_NULL_HANDLE) continue;
            if (matSet != lastMatSet) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mvLayout, 1, 1, &matSet, 0, nullptr);
                lastMatSet = matSet;
            }

            const u32 skinMode = (rmode <= 2u) ? 1u : (u32(rmode) - 1u);
            MVSkinPush pc{ curHudVP, prevHudVP, skinMode, u.baseBone, prevBase, (u32)u.boneCount, jitterNdcX, jitterNdcY };
            vkCmdPushConstants(cmd, s_mvLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

            VkBuffer vb = mesh->p_rm_Vertices->GetHandle();
            VkDeviceSize vbOff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOff);
            const SkinDraw skd = SkinLeafDraw(child, mesh, u.xform, true);
            vkCmdBindIndexBuffer(cmd, skd.ib, 0, mesh->iType);
            vkCmdDrawIndexed(cmd, skd.count, 1, skd.first, (s32)mesh->vBase, 0);
            ++nDraw;
        }
    }

    SetViewportDepth(cmd, ext, 0.0f, 1.0f);   // restore full-depth range

    static bool s_diagHud = false;
    if (!s_diagHud && nDraw) { s_diagHud = true; Msg("[VK Skinned] first HUD MV render: huds=%zu draws=%u", SF().uploadsHud.size(), nDraw); }
}

bool Skinned_AnyCasterInSphere(const Fvector& pos, float range)
{
    for (const SkelUpload& u : SF().uploads)
    {
        const Fsphere& bs = u.sphere;
        Fvector c; u.xform.transform_tiny(c, bs.P);
        const float rr = range + (bs.R > 0.f ? bs.R : 1.f);
        if (pos.distance_to_sqr(c) <= rr * rr) return true;
    }
    return false;
}

// VSM skinned casters â€” one entry per visible world skinned leaf, for vk_vsm to bin +
// rasterize into the virtual shadow atlas. Coarse skeleton world sphere per leaf (the
// binner over-covers slightly, which is harmless â€” extra pages just clip). HUD excluded.
void Skinned_CollectCasters(xr_vector<VsmSkinnedCaster>& out)
{
    if (!Init()) return;
    const float npcDist = ps_r_vsm_npc_dist;   // 0 = no cull (old behaviour)
    const bool standIn = !SF().uploadsShadow.empty();   // see Skinned_RenderShadow
    auto collect = [&](const xr_vector<SkelUpload>& list) {
    for (const SkelUpload& u : list)
    {
        if (u.noShadow && standIn) continue;
        const Fsphere& bs = u.sphere;
        Fvector c; u.xform.transform_tiny(c, bs.P);
        const float r = (bs.R > 0.f) ? bs.R : 1.f;
        // Distant NPCs cast a few-texel shadow â†’ skip the whole skeleton: saves the per-leaf
        // skinning + VSM binning AND keeps us under kMaxSkinned so NEAR NPCs are never dropped
        // when a crowd would otherwise overflow the cap. (Collect had NO cull before â†’ it
        // saturated at 256 leaves; log showed only ~40 actually cast.)
        if (npcDist > 0.f) {
            const float rr = npcDist + r;
            if (RV().vCameraPosition.distance_to_sqr(c) > rr * rr) continue;
        }
        for (auto* child : u.K->children)
        {
            VK_Render_Mesh* mesh = nullptr; u16 rmode = 0;
            if (!ResolveSkinnedLeaf(child, mesh, rmode)) continue;
            // Same rule as Skinned_RenderShadow: marks, glass and light sheets don't cast. Glass
            // did here -- the invisible bloodsucker (models\window body) threw a full VSM shadow.
            if (child->m_bEmissiveAdd || child->m_bModelGlass || child->m_bLitBlend) continue;
            const DrawSrc ds = ResolveDrawSrc(child, mesh, rmode, u.baseBone, u.boneCount);
            VsmSkinnedCaster sc{};
            sc.sphere_P     = c;            sc.sphere_R = r;
            sc.index_count  = mesh->iCount; sc.ib_first = mesh->iBase; sc.first_vertex = ds.firstVertex;
            sc.vb           = ds.vb;
            sc.ib           = mesh->p_rm_Indices->GetHandle();
            sc.iType        = mesh->iType;  sc.stride   = ds.stride;
            sc.base_bone    = ds.baseBone;  sc.bone_count = ds.boneCount;
            sc.skin_mode    = ds.skinMode;
            out.push_back(sc);
        }
    }
    };
    collect(SF().uploads);
    // The actor's own model (g_actor_shadow): never drawn, always a caster. It is
    // the nearest skeleton there is, so the npcDist cull can't drop it.
    collect(SF().uploadsShadow);
}

VkDescriptorSet       Skinned_GetBoneSet()       { return s_set; }
VkDescriptorSetLayout Skinned_GetBoneSetLayout() { return s_setLayout; }

void Skinned_BuildVertexInput(u32 stride, VkVertexInputBindingDescription& binding,
                              VkVertexInputAttributeDescription attrs[6])
{
    BuildSkinnedVI(stride, binding, attrs);
}

void Pass_Skinned(FrameContext& ctx)
{
    if (DynamicVisuals().empty() && HudVisuals().empty()) return;
    if (ctx.cmd == VK_NULL_HANDLE || !ctx.viewProj) return;
    if (!Init()) return;

    // (Bones: built by the PREPARE phase "Skin/bones" on the extract side.)

    VkCommandBuffer cmd = ctx.cmd;
    VkPipeline      lastPipe   = VK_NULL_HANDLE;
    VkDescriptorSet lastMatSet = VK_NULL_HANDLE;
    u32             lastWin    = kUnbound;
    u32 nDraw = 0, nHud = 0;

    // Per-frame env lighting (set 2). Pass_World already called EnvLight::Update this
    // frame (it runs before this pass), so just grab the current set; fall back to
    // updating here if this pass ever runs standalone.
    // ⚠No command buffer on purpose: this pass records inside dynamic rendering, where
    // the sky-probe prefilter's dispatch/blits would be illegal. The UBO fill is all
    // that is wanted here; the probe keeps last frame's content.
    if (EnvLight::GetCurrentSet() == VK_NULL_HANDLE) EnvLight::Update(CommandManager.GetCurrentFrame());
    VkDescriptorSet lightSet = EnvLight::GetCurrentSet();

    // World dynamics: bones are world-space â†’ push plain viewProj. Glass leaves wait for the
    // late Glass pass (Skinned_RenderGlassLate); the HUD keeps its own inline below.
    s_glassDeferred = false;
    DrawSkinnedList(cmd, SF().uploads, *ctx.viewProj, nDraw, lastPipe, lastMatSet, lastWin, lightSet, 0.0f, kGlassDefer);

    // First-person HUD: HUD-FOV projection (camera at origin) + near depth range so
    // hands/weapon render on top of the world. Restore the normal range afterwards.
    // hudMode=1 â†’ fragment uses flat, sun-direction-independent lighting (view-space bones).
    if (!SF().uploadsHud.empty()) {
        // Switch diagnostics for "the weapon/hands are not visible" (hudDraws > 0 yet nothing on screen). args2.txt: -nx_hud N
        //   1 = full depth range 0..1, 2 = world lighting path (hudMode 0), 3 = world viewProj instead of the HUD-FOV one
        static int s_hudTest = -1;
        if (s_hudTest < 0) {
            s_hudTest = 0;
            if (const char* q = strstr(Core.Params, "-nx_hud ")) s_hudTest = atoi(q + 8);
            if (s_hudTest) Msg("[VK Skinned] HUD test mode %d", s_hudTest);
        }
        SetViewportDepth(cmd, ctx.extent, 0.0f, s_hudTest == 1 ? 1.0f : 0.02f);
        DrawSkinnedList(cmd, SF().uploadsHud, s_hudTest == 3 ? *ctx.viewProj : RV().mFullTransform_hud, nHud, lastPipe, lastMatSet, lastWin, lightSet,
                        s_hudTest == 2 ? 0.0f : (ps_r_hud_vsun ? 2.0f : 1.0f));   // 10-10: 2 = r_hud_vsun (the sun shadow per vertex)
        SetViewportDepth(cmd, ctx.extent, 0.0f, 1.0f);
#ifdef __SWITCH__
        static u32 s_hudLog = 0;
        if (++s_hudLog % 127 == 1) {
            const Fmatrix& m = RV().mFullTransform_hud;
            Msg("[VK Skinned] HUD vp diag: m11=%.3f m22=%.3f m33=%.3f m34=%.3f m43=%.3f m44=%.3f | up0 xform pos=(%.2f,%.2f,%.2f) bones=%u",
                m._11, m._22, m._33, m._34, m._43, m._44,
                SF().uploadsHud[0].xform.c.x, SF().uploadsHud[0].xform.c.y, SF().uploadsHud[0].xform.c.z, (u32)SF().uploadsHud[0].boneCount);
        }
#endif
    }

    static bool s_diag = false;
    if (!s_diag) { s_diag = true; Msg("[VK Skinned] first Pass_Skinned: dynVis=%zu hud=%zu draws=%u hudDraws=%u (slot=%u)",
                                      SF().uploads.size(), SF().uploadsHud.size(), nDraw, nHud, CommandManager.GetCurrentFrame()); }
    // pulse diag (two-player invisibility hunt): proves per-frame whether the
    // main view collected any dynamics and whether their leaves really drew.
    // Odd modulus on purpose -- even throttles latch with two clients.
    static u32 s_pulse = 0;
    if (++s_pulse % 127 == 1)
    {
        Msg("[VK Skinned] pulse: dynVis=%zu hud=%zu ups=%zu draws=%u hudUps=%zu hudDraws=%u", DynamicVisuals().size(), HudVisuals().size(), SF().uploads.size(), nDraw, SF().uploadsHud.size(), nHud);
#ifdef __SWITCH__   // nfs3/4/8 instruments: three more lines per pulse, Switch log only
        // nfs3: the collection is a bare frustum query to the far plane (no portals / HOM / distance), so say how far
        // away what we skin and draw actually is.
        u32 bin[6] = {};
        const Fvector cam = RV().vCameraPosition;
        for (const SkelUpload& u : SF().uploads)
        {
            const float d = cam.distance_to(u.xform.c);
            bin[d < 15.f ? 0 : d < 30.f ? 1 : d < 60.f ? 2 : d < 120.f ? 3 : d < 250.f ? 4 : 5]++;
        }
        Msg("[VK Skinned] distance: <15m %u | <30m %u | <60m %u | <120m %u | <250m %u | farther %u", bin[0], bin[1], bin[2],
            bin[3], bin[4], bin[5]);
        // nfs4: colour-pass triangles at full detail vs with the R4 LOD window (lod=%d: r_skin_lod)
        u64 triFull = 0, triLod = 0;
        for (const SkelUpload& u : SF().uploads)
            for (auto* child : u.K->children)
            {
                VK_Render_Mesh* mesh = nullptr;
                u16 rmode = 0;
                if (!ResolveSkinnedLeaf(child, mesh, rmode)) continue;
                triFull += mesh->iCount / 3;
                triLod += SkinLeafCount(child, mesh, u.xform, false) / 3;
            }
        // 10-10: + the first-person hands/weapon (the A/B in hudwpn_v321: holstering the rifle took w.skinned 1.28 -> 0.12)
        u64 hudFull = 0, hudDrawn = 0;
        for (const SkelUpload& u : SF().uploadsHud)
            for (auto* child : u.K->children)
            {
                VK_Render_Mesh* mesh = nullptr;
                u16 rmode = 0;
                if (!ResolveSkinnedLeaf(child, mesh, rmode)) continue;
                hudFull += mesh->iCount / 3;
                hudDrawn += SkinLeafCount(child, mesh, u.xform, true) / 3;
            }
        Msg("[VK Skinned] triangles: full %llu | drawn %llu (lod %s) | HUD full %llu drawn %llu", (unsigned long long)triFull,
            (unsigned long long)triLod, SkinLodOn() ? "on" : "OFF", (unsigned long long)hudFull, (unsigned long long)hudDrawn);
        // nfs8: which LOD the data offers. m_lod = the model's separate LOD visual (R1/R4 swap the whole skeleton for it
        // beyond r_ssaLOD_A and skip its bones); PM leaves with >= 2 windows = the progressive LOD above can work.
        u32 withLod = 0, pmLeaves = 0, pmWin = 0, pmOwn = 0, stLeaves = 0, stWin = 0, stOwn = 0, lodType = 0;
        for (const SkelUpload& u : SF().uploads)
        {
            if (u.K->m_lod) { ++withLod; if (!lodType) lodType = u.K->m_lod->Type; }
            for (auto* child : u.K->children)
            {
                if (auto* pm = dynamic_cast<vkSkeletonX_PM*>(child)) { ++pmLeaves; if (pm->sw_count >= 2) ++pmWin; if (pm->skinLod && pm->skinLod->live(RV().frameIndex)) ++pmOwn; }
                else if (auto* st = dynamic_cast<vkSkeletonX_ST*>(child)) { ++stLeaves; if (st->st_lod_count >= 2) ++stWin; if (st->skinLod && st->skinLod->live(RV().frameIndex)) ++stOwn; }
            }
        }
        Msg("[VK Skinned] lod census: %zu skeletons, %u with an m_lod model (type %u) | leaves PM %u (%u with windows, %u with ours live), ST %u (%u with built windows, %u with ours live)",
            SF().uploads.size(), withLod, lodType, pmLeaves, pmWin, pmOwn, stLeaves, stWin, stOwn);
        // 09-10 v302: where the drawn triangles are -- v301 bridge drew 1.12M of 1.14M with our PM windows on. Leaves summed by
        // name (shared_str: one pointer per name), top 8 by drawn. Kind: P = PM with our windows live, p = PM without,
        // S = ST with windows (r_skin_st_lod or ours live), s = ST without.
        struct TopLeaf { const char* name; char kind; u32 n; u64 full, drawn; float R, dmin; };
        xr_vector<TopLeaf> top;
        for (const SkelUpload& u : SF().uploads)
            for (auto* child : u.K->children)
            {
                VK_Render_Mesh* mesh = nullptr;
                u16 rmode = 0;
                if (!ResolveSkinnedLeaf(child, mesh, rmode)) continue;
                const char* nm = child->dbg_name.c_str() ? child->dbg_name.c_str() : "?";
                char kind = '?';
                if (auto* pm = dynamic_cast<vkSkeletonX_PM*>(child)) kind = (pm->skinLod && pm->skinLod->live(RV().frameIndex)) ? 'P' : 'p';
                else if (auto* st = dynamic_cast<vkSkeletonX_ST*>(child)) kind = (st->st_lod_count >= 2 || (st->skinLod && st->skinLod->live(RV().frameIndex))) ? 'S' : 's';
                auto it = std::find_if(top.begin(), top.end(), [&](const TopLeaf& t) { return t.name == nm; });
                if (it == top.end()) { top.push_back({nm, kind, 0, 0, 0, u.K->vis.sphere.R, 1e9f}); it = top.end() - 1; }
                it->dmin = _min(it->dmin, RV().vCameraPosition.distance_to(u.xform.c));
                ++it->n;
                it->full += mesh->iCount / 3;
                it->drawn += SkinLeafCount(child, mesh, u.xform, false) / 3;
            }
        std::sort(top.begin(), top.end(), [](const TopLeaf& a, const TopLeaf& b) { return a.drawn > b.drawn; });
        string2048 line = "";
        for (size_t i = 0; i < top.size() && i < 8; ++i)
        {
            string256 one;
            xr_sprintf(one, "%s%c %s x%u %llu/%llu R%.1f %.0fm", i ? " | " : "", top[i].kind, top[i].name, top[i].n,
                       (unsigned long long)top[i].drawn, (unsigned long long)top[i].full, top[i].R, top[i].dmin);
            xr_strcat(line, one);
        }
        Msg("[VK Skinned] top leaves (drawn/full tris, model radius, nearest): %s", line);
#endif
    }
}

void Skinned_Destroy()
{
    s_footTracks.clear();   // keys are IKinematics* â€” drop before pointers recycle
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    for (auto& kv : s_pipelines) if (kv.second) vkDestroyPipeline(VulkanHW.m_Device, kv.second, nullptr);
    s_pipelines.clear();
    s_glassDistFS = VK_NULL_HANDLE;   // module owned by g_ShaderManager
    for (auto& kv : s_shadowPipelines) if (kv.second) vkDestroyPipeline(VulkanHW.m_Device, kv.second, nullptr);
    s_shadowPipelines.clear();
    for (auto& kv : s_prepassPipelines) if (kv.second) vkDestroyPipeline(VulkanHW.m_Device, kv.second, nullptr);
    s_prepassPipelines.clear();
    for (auto& kv : s_normalPipelines) if (kv.second) vkDestroyPipeline(VulkanHW.m_Device, kv.second, nullptr);
    s_normalPipelines.clear();
    for (auto& kv : s_motionPipelines) if (kv.second) vkDestroyPipeline(VulkanHW.m_Device, kv.second, nullptr);
    s_motionPipelines.clear();
    for (auto& kv : s_prepassMotionPipelines) if (kv.second) vkDestroyPipeline(VulkanHW.m_Device, kv.second, nullptr);
    s_prepassMotionPipelines.clear();
    s_shadowVS = VK_NULL_HANDLE;   // module owned by g_ShaderManager
    s_prepassVS = VK_NULL_HANDLE; s_prepassFS = VK_NULL_HANDLE;
    s_mvVS = VK_NULL_HANDLE; s_mvFS = VK_NULL_HANDLE; s_mvPreVS = VK_NULL_HANDLE;
    SF().curBoneMap.clear(); SF().prevBoneMap.clear();
    SF().curBoneMapHud.clear(); SF().prevBoneMapHud.clear();
    SF().uploads.clear(); SF().uploadsHud.clear(); SF().uploadsShadow.clear(); SF().uploadFrame = u32(-1);

    // Compute pre-skinning. SF().psMap MUST be dropped here: its keys are leaf
    // visuals that a level unload frees, and its values index a pool that no
    // longer exists.
    SF().psMap.clear(); SF().psRtLeaves.clear(); SF().psJobs.clear(); SF().psFrame = u32(-1);
    s_psLeaves = s_psVerts = s_psPeak = s_psOverflow = 0;
    if (s_psPipe)   { vkDestroyPipeline(VulkanHW.m_Device, s_psPipe, nullptr); s_psPipe = VK_NULL_HANDLE; }
    if (s_psLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_psLayout, nullptr); s_psLayout = VK_NULL_HANDLE; }
    if (s_psPool)   { vkDestroyDescriptorPool(VulkanHW.m_Device, s_psPool, nullptr); s_psPool = VK_NULL_HANDLE; }
    if (s_psSetL)   { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_psSetL, nullptr); s_psSetL = VK_NULL_HANDLE; }
    s_psBuf.Destroy();
    s_psPrevBuf.Destroy();
    s_psSet = VK_NULL_HANDLE;
    s_psInited = false; s_psFailed = false;

    if (s_mvLayout)  { vkDestroyPipelineLayout(VulkanHW.m_Device, s_mvLayout, nullptr); s_mvLayout = VK_NULL_HANDLE; }
    if (s_layout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_layout, nullptr); s_layout = VK_NULL_HANDLE; }
    VK::DynSets::Unregister(s_set);   // before the pool frees it
    if (s_pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    // Set 2 (EnvLight) is owned by vk_env_light.cpp â€” destroyed separately in DevRender::Destroy.
    s_boneSSBO.Destroy();
    s_boneMapped = nullptr;
    s_set = VK_NULL_HANDLE;
    s_vsUbo = s_shadowVSUbo = s_prepassVSUbo = s_normalVSUbo = VK_NULL_HANDLE;   // modules owned by g_ShaderManager
    s_inited = false; s_failed = false;
}

}  // namespace VK
