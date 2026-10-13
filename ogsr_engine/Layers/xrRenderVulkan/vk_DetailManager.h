// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - GPU-driven grass / detail-objects manager.
//
// Three-session port from monolith. THIS HEADER COVERS SESSION A ONLY:
//   - Asset load (level.details)
//   - Heightmap bake (collision tris → R32_SFLOAT image, ~1 m/texel)
//   - Slot data SSBO upload (DetailSlot → GpuSlotPacked, 32 B each)
//   - Object info SSBO upload (per-detail-type scale/radius/flags)
//
// Sessions B/C add: visible/indirect/atomic SSBOs, gen + trail compute
// pipelines, graphics pipeline, render path, optional bindless multi-draw.
// See `memory/vulkan_grass_port_plan.md` for the full plan.

#pragma once
#include "vk_render_view.h"
#include "vk_core.h"
#include "vk_buffer.h"
#include "vk_Detail.h"
#include "../xrRender/DetailFormat.h"

namespace VK
{

class CVulkanTexture;
struct FrameContext;


// ============================================================================
// Constants — pinned to monolith's values; do not tune without porting plan
// reference (`memory/vulkan_grass_port_plan.md`).
// ============================================================================
inline constexpr u32   dm_max_objects     = 64;
inline constexpr float dm_slot_size       = DETAIL_SLOT_SIZE;   // 2.0 m
inline constexpr u32   GPU_MAX_OBJ_TYPES  = 64;
// Output capacity — INITIAL only; the buffer AUTO-GROWS to demand at runtime
// (m_OutputCapacity, resized in Render when the gen reports per-type section
// overflow). The real constraint is the PER-TYPE section (capacity /
// numObjTypes, static equal split): a 21-type GAMMA detail set at density 0.25
// / radius 110 pushed the dominant type past 1.5M/21≈71k → the gen silently
// dropped whole regions ("поля без травы", 10-07). Now: overflow is counted
// per type (atomic slot GPU_MAX_OBJ_TYPES+objId), logged, and triggers a grow
// (one-off vkDeviceWaitIdle hitch) — a modder can pour ANY density/radius and
// the buffer follows. Ceiling below is a runaway guard, not a tuning knob.
inline constexpr u32   GPU_OUTPUT_CAPACITY = 1500000u;   // initial: 96 MB
inline constexpr u32   GPU_OUTPUT_CAP_MAX  = 8000000u;   // ceiling: 512 MB (logged if demand exceeds)
// Shadow-CASTER set capacity (second gen pass, distance-only cull ≤ ~50 m
// around the camera — no camera frustum, so a blade behind you still casts).
// 375k × 64 B = 24 MB; covers a ~68 m disc at max density.
inline constexpr u32   GPU_CASTER_CAPACITY = 375000u;
inline constexpr u32   MAX_GRASS_INTERACTORS = 4;

// ============================================================================
// Wire-format structs that hit the GPU (Session B will reference them).
// Sized exactly per monolith — keep static_asserts happy.
// ============================================================================

// Per-instance compacted by the gen compute, used as VB binding 1 in draw.
// Channel layout: row0..2 = 3×4 transform, color = (sun, giPacked, objId, hemi).
// color.g carries the per-tuft DDGI field sample as rgb9e5 BITS (packed by
// detail_generate.comp, fetched by detail.vert through an R32_UINT attribute
// view of the same bytes — r_grass_gi_inst). It held the trail press-down
// before that feature died; any future float use of .g is off the table while
// the field rides here.
struct DetailInstance
{
    Fvector4 row0, row1, row2;
    Fvector4 color;
};
static_assert(sizeof(DetailInstance) == 64, "DetailInstance must be 64 B");

// Per-slot packed payload (one per (size_x*size_z) entry).
struct GpuSlotPacked
{
    float y_base;       // DetailSlot::r_ybase()
    float y_height;     // DetailSlot::r_yheight()
    u32   ids;          // id0:8 | id1:8 | id2:8 | id3:8
    u32   lighting;     // c_dir:16 | c_hemi:16 (UNORM-style 16.16)
    u32   palette0;     // per-obj corner alphas, 4×8 bits
    u32   palette1;
    u32   palette2;
    u32   palette3;
};
static_assert(sizeof(GpuSlotPacked) == 32, "GpuSlotPacked must be 32 B");

// Per-detail-type metadata (mirrors CDetail render params).
struct GpuDetailObjInfo
{
    float minScale;
    float maxScale;
    float bvRadius;
    u32   flags;
};
static_assert(sizeof(GpuDetailObjInfo) == 16, "GpuDetailObjInfo must be 16 B");

// ============================================================================
// Push-constant blocks
// ============================================================================

// Compute generator push (208 B) — viewport / culling / slot range params.
struct DetailGenPushConstants
{
    Fmatrix  viewProj;          // 64
    Fvector4 frustumPlanes[6];  // 96  (left, right, bottom, top, near, far)
    Fvector4 cameraPos;         // 16  (xyz=eye, w=fTimeGlobal)
    Fvector4 fadeParams;        // 16  (fadeStartSq, fadeLimitSq, fadeRangeSq, density)
    Fvector4 casterParams;      // 16  (x>0 = CASTER pass: cull radiusSq, no frustum/HZB; y = capacity override)
    int      slotMinSX;
    int      slotMinSZ;
    int      slotCountX;
    int      slotCountZ;
};
static_assert(sizeof(DetailGenPushConstants) == 224, "Gen push must be 224 B");

// Generator UBO (64 B) — params that don't fit in push. Mapped host-visible
// and rewritten each frame (or on level load if unchanging).
struct DetailGenUBO
{
    float hmOriginX, hmOriginZ;
    float hmInvScaleX, hmInvScaleZ;
    u32   totalPositions;
    u32   numObjTypes;
    u32   outputCapacity;
    u32   posPerSlot;
    float dtOffsX, dtOffsZ;
    float dtSizeX, dtSizeZ;
    float slotSize;
    float detailHeight;
    u32   hmWidth, hmHeight;
};
static_assert(sizeof(DetailGenUBO) == 64, "Gen UBO must be 64 B");

// Graphics push (208 B) — SSFX wind / lighting / interactors. Layout mirror of
// shaders/detail_gfx_push.glsl — ONE header per side, keep them in step.
// (vHemiColor used to close this struct: filled and linearised every frame,
// read by no shader — the grass hemi side is lit through the sky SH + ambient
// UBO terms, not a pushed colour. Deleted 26-08.)
struct DetailGfxPushConstants
{
    Fmatrix  mViewProj;                            // 64
    Fvector4 wind_params;                          // 16  (wind_direction, wind_velocity, treeAmplitude, per-type wind scale)
    Fvector4 wsetup_grass;                         // 16  SSFX (animspeed, turbulence, push, wave)
    Fvector4 wind_anim;                            // 16  Environment.wind_anim (xyz drift) + w = minWindSpeed
    Fvector4 vConsts;                              // 16  (r_grass_aref, r_grass_asharp, 0, 0) — zw spare
    Fvector4 vInteractors[MAX_GRASS_INTERACTORS];  // 64  xyz=pos, w=radius (0=unused)
    Fvector4 vSunColor;                            // 16  env sun colour (rgb), pre-boosted; w unused
};
static_assert(sizeof(DetailGfxPushConstants) == 208, "Gfx push must be 208 B");

// Grass motion-vector push (VS-only) — pairs with detail_motion.vert. Projects the
// cur AND prev wind-displaced pose so blades swaying in the wind carry their TRUE
// screen motion; the fullscreen MV pass only reconstructs camera reprojection from
// depth and misses the sway. wind_params.w = per-type wind scale (patched per draw,
// shared by cur+prev since DO_NO_WAVING is frame-stable). 208 B.
struct DetailMotionPushConstants
{
    Fmatrix  curVP;             // 0    this frame's view-proj, UNJITTERED (jitter-free MV)
    Fmatrix  prevVP;            // 64   previous frame's view-proj, UNJITTERED
    Fvector4 wind_params;       // 128  cur (.w = per-type wind scale, patched per draw)
    Fvector4 wsetup_grass;      // 144  SSFX grass tunables (constant frame-to-frame)
    Fvector4 wind_anim;         // 160  cur drift (Environment.wind_anim) + w = minWindSpeed
    Fvector4 wind_params_prev;  // 176  previous frame's wind params
    Fvector4 wind_anim_prev;    // 192  previous frame's drift
    Fvector4 jitter;            // 208  xy = this frame's sub-pixel jitter (D3D-NDC), re-applied to gl_Position
};
static_assert(sizeof(DetailMotionPushConstants) == 224, "Grass MV push must be 224 B");

// HZB build push (32 B) — matches hzb_build.comp.glsl. srcMip/dstMip select
// levels; isFirstPass=1 reads the depth buffer, =0 reads the previous HZB mip.
struct HZBBuildPush
{
    s32 srcW, srcH;     // ivec2 srcSize
    s32 dstW, dstH;     // ivec2 dstSize
    u32 srcMip, dstMip;
    u32 isFirstPass;
    u32 _pad;
};
static_assert(sizeof(HZBBuildPush) == 32, "HZB build push must be 32 B");

// ============================================================================
// CDetailManager — Sessions A + B (B-min: gen compute + forward draw).
// Trail map / character interaction / bindless multi-draw deferred.
// ============================================================================
class CDetailManager
{
public:
    struct SSwingValue
    {
        float rot1 = 0.0f, rot2 = 0.0f;
        float amp1 = 0.0f, amp2 = 0.0f;
        float speed = 0.0f;
        void lerp(const SSwingValue& v1, const SSwingValue& v2, float factor);
    };

    typedef svector<VK::CDetail*, dm_max_objects> DetailVec;

public:
    // ----- Asset data -------------------------------------------------------
    IReader*     dtFS    = nullptr;
    DetailHeader dtH{};
    DetailSlot*  dtSlots = nullptr;        // pointer into VFS-mapped chunk 2
    DetailSlot   DS_empty{};

    DetailVec    objects;

    // ----- Wind animation params (env-driven, used in Session B render) ----
    SSwingValue  swing_desc[2]{};          // [0]=normal, [1]=fast
    SSwingValue  swing_current{};
    float        m_time_rot_1     = 0.0f;
    float        m_time_rot_2     = 0.0f;
    float        m_time_pos       = 0.0f;
    float        m_global_time_old = 0.0f;
    float        fade_distance    = 60.0f;

    // ----- Grass-shadow caster access (read-only) ---------------------------
    // VSM/spot/cascade/point-cube passes rasterize NEAR grass casters from the
    // dedicated CASTER instance buffer (1 frame stale — grass gen runs after the
    // shadow passes in the frame). The caster set is a second gen dispatch with
    // NO camera-frustum/HZB cull (distance-only): a blade behind the camera
    // still casts a shadow you can see (campfire dapples on the wall you walk
    // toward). Falls back to the visible buffer if the caster set is absent.
    VkBuffer Vsm_VisibleSSBO()  const;     // caster SSBO (visible SSBO fallback)
    VkBuffer Vsm_IndirectBuf()  const;     // caster indirect (visible fallback)
    u32      Vsm_TypeCount()    const;
    u32      Vsm_SectionSize()  const;     // caster capacity / max(types,1) — per-type stride
    u32      Vsm_VertexStride() const;     // grass mesh binding-0 stride (sizeof CDetail::Vertex)
    bool     Vsm_TypeMesh(u32 i, VkBuffer& vb, VkBuffer& ib, u32& indexCount) const;
    VkDescriptorSetLayout Vsm_GfxSetLayout() const;    // diffuse-sampler set layout (for the grass-page alpha test)
    VkDescriptorSet       Vsm_TypeDiffuseSet(u32 i) const;
    // Wind push block for external grass casters (spot shadow map): last
    // PrepareFrame's values — 1 frame stale, same as the instance buffer.
    void  Vsm_WindPush(Fvector4& wind, Fvector4& wsetup, Fvector4& anim) const;
    float Vsm_TypeWindScale(u32 i) const;   // 0 for DO_NO_WAVING types (r_grass_nowave), else 1

private:
    // ----- Session B per-frame state ---------------------------------------
    DetailGfxPushConstants m_GfxConstants{};
    struct FrameState
    {
        int  slotMinSX     = 0;
        int  slotMinSZ     = 0;
        int  slotCountX    = 0;
        int  slotCountZ    = 0;
        u32  totalPositions = 0;
        u32  groupCount    = 0;
        u32  posPerSlot    = 25;     // (d_size+1)² for default density 0.6 → d_size=4
        u32  d_size        = 4;
        bool valid         = false;
    } m_Frame;

    // ----- Session A GPU resources -----------------------------------------

    // R32F heightmap baked from static collision triangles. Sentinel
    // value 9000.0f marks texels no triangle covered (compute shader
    // tests `abs(h) < 9000` and falls back to the slot's y_base).
    VkImage         m_HeightmapImage   = VK_NULL_HANDLE;
    VmaAllocation   m_HeightmapAlloc   = VK_NULL_HANDLE;
    VkImageView     m_HeightmapView    = VK_NULL_HANDLE;
    VkSampler       m_HeightmapSampler = VK_NULL_HANDLE;
    u32             m_HeightmapW       = 0;
    u32             m_HeightmapH       = 0;
    float           m_HMOriginX        = 0.0f;
    float           m_HMOriginZ        = 0.0f;
    float           m_HMWorldSizeX     = 0.0f;
    float           m_HMWorldSizeZ     = 0.0f;

    // Packed slot payload — size_x * size_z * 32 B.
    VK::CVulkanBuffer* m_SlotDataSSBO  = nullptr;
    u32                m_TotalSlots    = 0;

    // Per-type metadata — objects.size() * 16 B.
    VK::CVulkanBuffer* m_ObjInfoSSBO   = nullptr;

    // ----- Session B GPU resources -----------------------------------------

    // Compute output: per-instance compacted draw stream. Bound as VB
    // binding 1 (INSTANCE rate) by the graphics draw.
    VK::CVulkanBuffer* m_VisibleSSBO   = nullptr;     // m_OutputCapacity × 64 B (auto-grows)
    VK::CVulkanBuffer* m_IndirectCmdBuf= nullptr;     // 64 × 20 B
    VK::CVulkanBuffer* m_AtomicCounters= nullptr;     // 132 × 4 B ([64..127] = per-type overflow-drop counters)
    VK::CVulkanBuffer* m_GenUBO        = nullptr;     // 64 B host-visible
    // Live output capacity (instances). Starts at GPU_OUTPUT_CAPACITY; Render
    // grows m_VisibleSSBO when the gen reports section overflow (grass holes).
    u32                m_OutputCapacity  = GPU_OUTPUT_CAPACITY;
    u32                m_PendingCapacity = 0;          // grow request, applied at next Render start
    // Usage/overflow diagnostics: host readback of counters[0..127] (used +
    // dropped per type, a few frames stale), logged throttled from Render.
    VK::CVulkanBuffer* m_OverflowRB    = nullptr;     // 128 × 4 B host
    u32*               m_OverflowPtr   = nullptr;
    u32                m_OverflowLastLog = 0;
    u32                m_UsageLastLog    = 0;

    // Shadow-CASTER instance set (second gen dispatch, distance-only cull —
    // see the Vsm_* getter comment). Read by the spot/cascade/cube/VSM passes.
    VK::CVulkanBuffer* m_CasterSSBO       = nullptr;  // 24 MB
    VK::CVulkanBuffer* m_CasterIndirectBuf= nullptr;  // 64 × 20 B
    VK::CVulkanBuffer* m_CasterAtomic     = nullptr;  // 132 × 4 B

    // Compute (generator) pipeline.
    VkPipeline             m_GenPipeline      = VK_NULL_HANDLE;
    VkPipeline             m_GenPipelineLean  = VK_NULL_HANDLE;   // nfs34 Switch: slot pre-cull, no GI, aggregated append
    VkPipeline             m_GenPipelineLean2 = VK_NULL_HANDLE;   // nfs38 Switch: lean without local memory / HZB code
    // nfs40 Switch: R1-style tuft cache -- bake the slots entering the window once, cull the cached tufts per frame.
    VkPipeline             m_CacheBakePipeline   = VK_NULL_HANDLE;
    VkPipeline             m_CacheCullPipeline   = VK_NULL_HANDLE;
    VkPipelineLayout       m_CachePipelineLayout = VK_NULL_HANDLE;   // { gen set 0, EnvLight, cache set 2 }
    VkDescriptorSetLayout  m_CacheDescLayout     = VK_NULL_HANDLE;
    VkDescriptorPool       m_CacheDescPool       = VK_NULL_HANDLE;
    VkDescriptorSet        m_CacheDescSet        = VK_NULL_HANDLE;
    VK::CVulkanBuffer*     m_CacheEntries  = nullptr;   // W*W*posPerSlot x 32 B
    VK::CVulkanBuffer*     m_CacheHeaders  = nullptr;   // W*W x 16 B
    VK::CVulkanBuffer*     m_CacheBakeList = nullptr;   // kCacheRing x W*W x 16 B, host-visible
    u32*                   m_CacheBakePtr  = nullptr;
    u32                    m_CacheW = 0, m_CachePPS = 0, m_CacheRing = 0;
    xr_vector<s32>         m_CacheCellCoord;            // per cell: the (sx, sz) baked there
    bool CacheEnsure(u32 W, u32 pps);
    void CacheDestroy();
    VkPipeline             m_CountersPipeline = VK_NULL_HANDLE;   // nfs35 Switch: counter reset / publish without transfers
    VkPipelineLayout       m_GenPipelineLayout= VK_NULL_HANDLE;
    VkDescriptorSetLayout  m_GenDescLayout    = VK_NULL_HANDLE;
    VkDescriptorPool       m_GenDescPool      = VK_NULL_HANDLE;
    VkDescriptorSet        m_GenDescSet       = VK_NULL_HANDLE;
    VkDescriptorSet        m_CasterDescSet    = VK_NULL_HANDLE;   // caster SSBO/atomics at bindings 3/4

    // 1×1 placeholder textures for HZB / TrailMap bindings (B-min skips
    // both occlusion culling and footprint memory). HZB white = depth 1.0
    // never culls; TrailMap black = no press-down.
    VkImage         m_DummyHZBImage   = VK_NULL_HANDLE;
    VmaAllocation   m_DummyHZBAlloc   = VK_NULL_HANDLE;
    VkImageView     m_DummyHZBView    = VK_NULL_HANDLE;
    VkImage         m_DummyTrailImage = VK_NULL_HANDLE;
    VmaAllocation   m_DummyTrailAlloc = VK_NULL_HANDLE;
    VkImageView     m_DummyTrailView  = VK_NULL_HANDLE;

    // ----- HZB: Hierarchical-Z occlusion pyramid for grass culling ---------
    // Built each frame from the scene depth buffer (after Pass_World) via a
    // max-reduce mip chain; the gen compute samples it (binding 6) to drop
    // instances hidden behind world geometry. mip0 is half the depth res.
    VkImage         m_HZBImage        = VK_NULL_HANDLE;
    VmaAllocation   m_HZBAlloc        = VK_NULL_HANDLE;
    VkImageView     m_HZBView         = VK_NULL_HANDLE;   // full-mip sampled (gen binding 6 + build src)
    xr_vector<VkImageView> m_HZBMipViews;                 // per-mip single-level storage (build dst)
    VkSampler       m_HZBSampler      = VK_NULL_HANDLE;   // nearest, clamp-to-edge, all mips
    VkImageView     m_DepthSampleView = VK_NULL_HANDLE;   // DEPTH-aspect view of swapchain depth (first pass src)
    u32             m_HZBWidth        = 0;                // mip0 dims
    u32             m_HZBHeight       = 0;
    u32             m_HZBMipCount     = 0;
    VkExtent2D      m_HZBDepthExtent  = { 0, 0 };         // depth extent HZB was sized for (resize detect)
    bool            m_bHZBLayoutInit  = false;            // HZB transitioned UNDEFINED→GENERAL once
    u32             m_HZBBuiltFrame   = 0xFFFFFFFFu;      // RV().frameIndex the pyramid was last built (build-once-per-frame guard)
    u32             m_HZBGeneration   = 0;                // ++ on every (re)creation — descriptor-cache key (handles can be recycled)

    // HZB build compute pipeline. One descriptor set per mip pass.
    VkPipeline             m_HZBPipeline       = VK_NULL_HANDLE;
    VkPipelineLayout       m_HZBPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout  m_HZBDescLayout     = VK_NULL_HANDLE;
    VkDescriptorPool       m_HZBDescPool       = VK_NULL_HANDLE;
    xr_vector<VkDescriptorSet> m_HZBDescSets;

    // Graphics (forward draw) pipeline. Two FS twins over the same layout /
    // vertex input / VS: m_GfxPipeline = per-pixel probe walk (detail.frag.spv),
    // m_GfxPipelineInst = per-tuft field from the instance record
    // (detail_giinst.frag.spv, r_grass_gi_inst 1 — the default). Both built at
    // load; the draw picks by cvar, so the A/B flips live over identical
    // resources (no recreation — the parked-SH lesson).
    VkPipeline             m_GfxPipeline       = VK_NULL_HANDLE;
    VkPipeline             m_GfxPipelineInst   = VK_NULL_HANDLE;
    VkPipeline             m_GfxPipelineR1     = VK_NULL_HANDLE;   // nfs22 r_grass_r1 1: R1 grass FS (detail_r1.frag)
    VkPipeline             m_GfxPipelineR1V    = VK_NULL_HANDLE;   // nfs32 r_grass_r1 2: light/fade/fog per vertex (detail_r1v)
    VkPipelineLayout       m_GfxPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout  m_GfxDescLayout     = VK_NULL_HANDLE;
    VkDescriptorPool       m_GfxDescPool       = VK_NULL_HANDLE;
    // One descriptor set per detail type (single sampler binding = diffuse).
    xr_vector<VkDescriptorSet> m_GfxDescSets;

    // Motion-vector (wind-sway) overlay pipeline — re-draws the visible grass into
    // the MV target with detail_motion.{vert,frag}, reusing the gfx descriptor sets
    // (set0 = diffuse + s_waves). Lazy-created on first RenderMotion. Depth-tests
    // (LEQUAL, no write) against the scene depth the forward grass pass wrote.
    VkPipeline        m_MotionPipeline       = VK_NULL_HANDLE;
    VkPipelineLayout  m_MotionPipelineLayout = VK_NULL_HANDLE;
    // Previous-frame wind for the MV pass (the sway delta lives here). Rolled in
    // PrepareFrame before m_GfxConstants is overwritten with this frame's wind.
    Fvector4 m_MvWindParamsPrev{};
    Fvector4 m_MvWindAnimPrev{};
    bool     m_MvWindPrevValid = false;

    // Per-detail-type loaded diffuse textures (one entry per `objects[]`) — these
    // are REFERENCES and repeat: detail objects overwhelmingly share one level atlas
    // (Pripyat: 29 objects, ONE `build_details.dds`). Ownership lives in the list
    // below, which holds each distinct .dds once; loading per object cost 29 copies
    // of the same image = 298 MB of the level's VRAM.
    xr_vector<VK::CVulkanTexture*> m_DetailTextures;
    xr_vector<VK::CVulkanTexture*> m_DetailTexOwned;   // unique textures — the ones to free
    VkSampler                      m_DetailSampler = VK_NULL_HANDLE;
    VK::CVulkanTexture*            m_WaveTex       = nullptr;  // SSFX wind flow map (s_waves, wind_wave.dds)

    bool            m_bCreated         = false;

public:
    CDetailManager();
    virtual ~CDetailManager();

    void Load();          // open level.details, bake heightmap, upload SSBOs
    void Unload();
    bool IsLoaded() const { return m_bCreated; }
    void GenerateEarly(VK::FrameContext& ctx);   // nfs41: the generation half of Render, at the frame start
    void RenderDraw(VK::FrameContext& ctx, u32 nObj, u32 sectionSize);   // nfs41: the draw half of Render
    bool m_GenOnly = false;                      // nfs41: Render stops before the draw
    u32  m_GenEarlyFrame = 0xFFFFFFFFu;          // nfs41: frame whose generation already ran
    int  GenMode() const;                        // r_grass_gen, clamped to the pipelines that exist (+ nx windows)
    void DumpNxPipeStats();                 // nfs37: driver statistics of the grass pipelines (r_pipe_stats)

    // ----- Session B render entry ------------------------------------------
    // PrepareFrame fills m_GfxConstants + m_Frame from camera/wind/density.
    // Render does the full per-frame pipeline (clear → barrier → trail-skip
    // → gen dispatch → barrier → atomic→indirect copy → barrier → draw).
    // Called from CRender::Render() between Pass_World and Pass_Sky.
    void Render(struct VK::FrameContext& ctx);

    // Wind-sway motion-vector overlay. Called by VK::MotionVec::ExecuteDynamic
    // INSIDE its already-begun MV render pass (MV target + scene depth bound,
    // negative-height full-depth viewport). curVP/prevVP are UNJITTERED (jitter-free
    // MV); jitterNdcX/Y (D3D-NDC, 0 when DLSS off) is re-applied to gl_Position so
    // grass depth still bit-matches the jittered forward draw. No-op until grass loaded.
    void RenderMotion(const struct VK::FrameContext& ctx, const Fmatrix& curVP, const Fmatrix& prevVP,
                      float jitterNdcX, float jitterNdcY);

    // World-slot lookup. Returns DS_empty for out-of-bounds queries
    // (id0..3 = ID_Empty marker).
    DetailSlot& QueryDB(int sx, int sz);

    // ----- Hi-Z occlusion pyramid sharing (Phase A: WorldGPU::CullColor) -----
    // Pass_World builds the pyramid from THIS frame's PREPASS depth (before its
    // color pass) so the GPU-driven static color pass can occlusion-cull against
    // it. The build is frame-stamped (m_HZBBuiltFrame) → grass's own BuildHZB at
    // the top of Render() then no-ops, so the pyramid is built once per frame
    // regardless of which caller runs first. HZB lives in VK_IMAGE_LAYOUT_GENERAL.
    // `force` re-reduces the pyramid even though it was already built this frame.
    // Exactly one caller needs it: the two-phase prepass (r_hzb_prepass) builds
    // once from the partial phase-1 depth to drive the late cull, then again from
    // the finished depth so the LATER consumers of this frame's pyramid — the
    // tree cull, grass gen, lod_cull — see the complete scene rather than a
    // half-drawn one. Two reductions cost ~0.09 ms each (measured 13-08).
    void        BuildHZBForFrame(struct VK::FrameContext& ctx, bool force = false)
    {
        if (force) m_HZBBuiltFrame = 0xFFFFFFFFu;
        BuildHZB(ctx);
    }
    bool        HZBReady()   const { return m_HZBImage != VK_NULL_HANDLE && m_HZBView != VK_NULL_HANDLE && m_HZBPipeline != VK_NULL_HANDLE; }
    // "Was the pyramid actually built for THIS frame?" — HZBReady() only says the
    // resources exist. Consumers that run LATER in the frame (the tree cull) and
    // deliberately do NOT build it themselves must check this: sampling a pyramid
    // left over from the previous frame would occlusion-cull against a stale
    // camera. Pass_World builds it when r_hzb_cull + the GPU world set are on.
    bool        HZBBuiltThisFrame() const { return m_HZBBuiltFrame == RV().frameIndex; }
    // "Does the pyramid still hold LAST frame's finished depth, at the size the
    // camera is rendering now?" — the precondition for the two-phase prepass cull
    // (r_hzb_prepass), which reads the pyramid BEFORE anything overwrites it.
    // Three ways it can be false, all of them ordinary: the very first frame after
    // a load (m_HZBBuiltFrame is still the 0xFFFFFFFF sentinel — checked
    // explicitly, because sentinel+1 == 0 would read as "valid" on frame 0), a
    // frame where nobody built it, and a resolution change (DLSS render-res flip
    // or a window resize) where the stored pyramid describes a different screen.
    // A false here is not an error: phase 1 simply runs with no occlusion test and
    // comes out identical to the single-phase frustum cull.
    // (defined next to BuildHZB — it compares against the live swapchain depth
    // extent, and this header does not see vk_swapchain.h)
    bool        HZBPrevUsable() const;
    VkImageView HZBView()    const { return m_HZBView; }
    VkSampler   HZBSampler() const { return m_HZBSampler; }
    // Raw pyramid for the CPU readback (vk_hzb_readback.cpp): R32F, layout GENERAL, mip0 = depth/2, mip k = mip0 >> k.
    VkImage     HZBImage()    const { return m_HZBImage; }
    u32         HZBWidth()    const { return m_HZBWidth; }
    u32         HZBHeight()   const { return m_HZBHeight; }
    u32         HZBMipCount() const { return m_HZBMipCount; }
    // 1x1 white (depth 1.0 = "nothing occludes") stand-in. Consumers that keep a
    // permanently-declared HZB binding point it here when occlusion is off — a
    // combined-image-sampler left unbound is UB on dispatch even if the branch
    // reading it is never taken.
    VkImageView DummyHZBView() const { return m_DummyHZBView; }
    // Increments on every pyramid (re)creation — see the note at the bump site.
    u32         HZBGeneration() const { return m_HZBGeneration; }

private:
    // Session A
    void BakeHeightmap();
    void UploadSlotData();
    void UploadObjInfo();

    // Session B
    void CreateGpuBuffers();
    void DestroyGpuBuffers();
    void CreateDummyTextures();
    void DestroyDummyTextures();
    // HZB occlusion pyramid (sized to depth extent; recreated on resize).
    void CreateHZB(u32 depthW, u32 depthH);
    void DestroyHZB();
    void BuildHZB(VK::FrameContext& ctx);   // per-frame: depth → Hi-Z mip chain
    void CreateGpuGenPipeline();
    void DestroyGpuGenPipeline();
    void CreateGfxPipeline();
    void DestroyGfxPipeline();
    void CreateMotionPipeline();    // lazy grass MV overlay pipeline (detail_motion.*)
    void DestroyMotionPipeline();
    void LoadDetailTextures();
    void DestroyDetailTextures();

    void PrepareFrame(const VK::FrameContext& ctx);
public:
    // RENDER_THREAD_PLAN step 8: the player + 3 nearest NPCs are found by walking the live object list --
    // extract-side work (PREPARE phase). PrepareFrame copies the result.
    void ExtractInteractors();
private:
    // Two-slot pipeline: per frame slot (ExtractInteractors on the main thread fills its own, PrepareFrame on the render
    // thread copies its frame's).
    struct ExtractedInteractors { Fvector4 v[MAX_GRASS_INTERACTORS]; u32 frame = 0xFFFFFFFFu; };
    RT::PerSlot<ExtractedInteractors> m_Interactors;
    void ExtractFrustumPlanes(const Fmatrix& vp, Fvector4 planes[6]) const;
    void UpdateGenDescriptors();   // rebinds when textures might rotate
};

}  // namespace VK
