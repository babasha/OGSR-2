// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// GPU-Driven Particles — Phase 1 (proof of life). One hardcoded effect
// (Source + Gravity + KillOld) simulated and drawn entirely on the GPU with a
// resident pool + free-list. See gpu_particles_roadmap.md.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_rendering.h"     // VK::RenderingBuilder
#include "vk_gpu_particles.h"
#include <atomic>
#include <mutex>
#include "vk_buffer.h"
#include "vk_shaders.h"
#include "vk_profiler.h"
#include "vk_command_buffer.h"
#include "vk_barriers.h"
#include "vk_pipeline_cache.h"
#include "vk_compute_util.h"     // VK::MakePipelineLayout / CreateComputePipeline
#include "vk_gfx_pipeline.h"     // VK::GfxPipelineBuilder
#include "vk_swapchain.h"
#include "vk_scene_color.h"
#include "vk_pass_particles.h"   // ParticlePass::GetTextureView — bindless sprite views
#include "vk_volumetrics.h"      // Vol:: froxel scatter probe (Stage-0 smoke lighting)
#include "HW_Vulkan.h"
#include "vk_core.h"
#include "../../xr_3da/device.h"

extern int ps_r_gpu_particles;
extern int ps_r_gpu_particles_max;
extern int ps_r_gpu_particles_sort;   // Phase 4: depth-sort alpha smoke (back-to-front)
extern float ps_r_vol_smoke;        // Stage-0 smoke light-probe strength
extern float ps_r_vol_smoke_clamp;  // radiance clamp (smoke doesn't blow to white)

// World-emitter feed: walks the live .pe particle visuals and appends one
// EmitterSample per playing smoke effect. Defined in vk_gpu_particles_feed.cpp
// (which carries the heavy PS/visual includes the clean module avoids).
namespace VK { namespace GPUParticles { struct EmitterSample; } }
extern void VK_GP_CollectWorldEmitters(xr_vector<VK::GPUParticles::EmitterSample>& out, float dt);

namespace VK { namespace GPUParticles {

namespace {
    constexpr u32 kFramesInFlight     = CVulkanCommandManager::FRAMES_IN_FLIGHT;
    constexpr u32 kDefaultMaxParticles = 1u << 16;   // 64K — plenty for the Phase-1 test
    constexpr u32 kNumBindings         = 11u;  // 8 = progAlive (#5), 9 = sort scratch (Phase 4), 10 = kill requests
    constexpr u32 kSortBuckets         = 512u; // must match gp_common.glsl GP_SORT_BUCKETS
    constexpr u32 kMaxKills            = 64u;  // emitter kill requests per frame (excess stays queued)
    constexpr u32 kMaxEmitPerFrame     = 1024u;      // safety cap on a frame's total spawn burst
    constexpr u32 kMaxEmitPerEmitter   = 256u;       // per-emitter burst cap
    constexpr u32 kMaxPrograms         = 64u;        // per-defId program registry capacity
    constexpr u32 kMaxEmitters         = 256u;       // active emitters (incl. camera) per frame

    // ---- State -------------------------------------------------------------
    bool s_inited     = false;
    bool s_failed     = false;
    bool s_poolInited = false;        // gp_init ran once (lazy, first dispatch)
    u32  s_maxP       = kDefaultMaxParticles;

    // SSBOs.
    CVulkanBuffer* s_pool      = nullptr;   // GPUParticle[maxP]
    CVulkanBuffer* s_counters  = nullptr;   // [0]=freeCount, [1]=aliveCount
    CVulkanBuffer* s_aliveList = nullptr;   // uint[maxP]
    CVulkanBuffer* s_indirect  = nullptr;   // VkDrawIndirectCommand
    CVulkanBuffer* s_freeList  = nullptr;   // uint[maxP]
    CVulkanBuffer* s_program   = nullptr;   // Program[kMaxPrograms]
    CVulkanBuffer* s_spawnReq[kFramesInFlight] = {};  // SpawnRequest[kMaxEmitters], per-frame
    CVulkanBuffer* s_texInfo   = nullptr;   // TexInfo[kMaxPrograms] (per-program atlas meta)
    CVulkanBuffer* s_progAlive = nullptr;   // uint[kMaxPrograms] — GPU-side alive count per program (#5)
    CVulkanBuffer* s_sort      = nullptr;   // uint[kSortBuckets + maxP] — Phase 4 histogram + unsorted copy
    CVulkanBuffer* s_killReq[kFramesInFlight] = {};  // KillRequest[kMaxKills], per-frame host-visible

    // Must match gp_common.glsl KillRequest (32 B std430).
    struct KillRequest {
        float posRadius[4];    // xyz = emitter position, w = radius SQUARED
        u32   program;
        u32   _kp[3];
    };
    static_assert(sizeof(KillRequest) == 32, "KillRequest 32 B std430");

    // Pending emitter kills (queued from game/render threads, drained per frame).
    std::mutex              s_killMx;
    xr_vector<KillRequest>  s_pendingKills;

    // ---- Bindless sprite textures (Phase 3 #5) -----------------------------
    // One COMBINED_IMAGE_SAMPLER array (slot = program index), written
    // append-only as effects register — a freshly written slot is never in use
    // by an in-flight frame, so no UPDATE_AFTER_BIND is needed. Requires
    // descriptor indexing; without it the draw stays on the procedural circle.
    bool                  s_useTextures = false;
    VkDescriptorSetLayout s_texSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      s_texPool      = VK_NULL_HANDLE;
    VkDescriptorSet       s_texSet       = VK_NULL_HANDLE;
    VkSampler             s_texSampler   = VK_NULL_HANDLE;

    // ---- Froxel volumetric light-probe (Stage-0 smoke lighting) ------------
    // set 2 = the Vol scatter volume (sampler3D). Only on the bindless path;
    // re-pointed at Vol's view when the volumes regenerate (Generation bumps).
    VkDescriptorSetLayout s_volSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet       s_volSet       = VK_NULL_HANDLE;
    u32                   s_volBoundGen  = 0xFFFFFFFFu;   // != any real Generation()

    // ---- Per-defId program registry (Phase 3 step 1) -----------------------
    // s_program holds Program[kMaxPrograms]. Slot 0 = authored campfire;
    // gp_mirror/gp_spawn register real .pe effects into later slots. Each
    // particle stores its slot in defId so gp_simulate runs the right program.
    shared_str s_progName[kMaxPrograms];    // [0] = "<campfire>"
    float      s_progRate[kMaxPrograms] = {};
    float      s_progLife[kMaxPrograms] = {};   // particle lifetime (fallback budget gating)
    u32        s_progMaxP[kMaxPrograms] = {};   // authored per-INSTANCE budget (CPEDef::m_MaxParticles) — #5
    std::atomic<s32> s_progInstances[kMaxPrograms] = {};  // LIVE claimed effect objects per program (#5 cap basis)
    u32        s_progCount     = 0;

    // Deferred instance releases: a destroyed object's budget stays claimed
    // until its ghost particles could have died (the life cap), otherwise the
    // freed units are snapped up by other instances' spawn requests and the
    // effect starves briefly when its object is recreated.
    struct PendingRelease { int slot; float when; };
    std::mutex                 s_releaseMx;      // destructor (game thread) vs drain (render thread)
    xr_vector<PendingRelease>  s_pendingRelease;

    // Auto-routing (Phase 3 #3) won't move an effect onto the shared GPU pool if
    // its steady-state budget (rate × life) exceeds this — area fog / persistent
    // anomaly fields (e.g. fog_ground_base = 1024/s × 180s ≈ 184k) would saturate
    // the 64K pool and starve every other effect. They stay on the CPU path.
    constexpr float kRouteBudget = 8192.0f;

    // ---- Emitters (Phase 3 step 2) -----------------------------------------
    // Each emitter feeds one spawn request per frame. [0] is the camera-pinned
    // emitter (set by gp_mirror); the rest are persistent world emitters placed
    // by gp_spawn. Each carries its own fractional emit-rate accumulator.
    struct Emitter {
        u32      program      = 0;
        bool     followCamera = false;
        bool     enabled      = true; // camera emitter starts OFF (test only)
        Fvector  pos          = {};   // world position when !followCamera
        float    accum        = 0.0f; // fractional emit-rate carry
    };
    xr_vector<Emitter> s_emitters;     // [0] = camera emitter (off unless gp_mirror)

    // Pipelines.
    VkPipeline s_pipeInit  = VK_NULL_HANDLE;
    VkPipeline s_pipeReset = VK_NULL_HANDLE;
    VkPipeline s_pipeEmit  = VK_NULL_HANDLE;
    VkPipeline s_pipeSim   = VK_NULL_HANDLE;
    VkPipeline s_pipeBuild   = VK_NULL_HANDLE;
    VkPipeline s_pipeDraw    = VK_NULL_HANDLE;   // alpha-blend billboards
    VkPipeline s_pipeDrawAdd = VK_NULL_HANDLE;   // additive billboards (fire/sparks)
    VkPipeline s_pipeSortHist    = VK_NULL_HANDLE;   // Phase 4: depth-bucket histogram
    VkPipeline s_pipeSortScan    = VK_NULL_HANDLE;   // Phase 4: exclusive prefix sum
    VkPipeline s_pipeSortScatter = VK_NULL_HANDLE;   // Phase 4: back-to-front scatter

    // Layouts.
    VkPipelineLayout s_compLayout = VK_NULL_HANDLE;
    VkPipelineLayout s_drawLayout = VK_NULL_HANDLE;

    // Descriptors.
    VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool      s_poolDesc  = VK_NULL_HANDLE;
    VkDescriptorSet       s_set[kFramesInFlight] = {};

    // ---- #6 media splat (GPU smoke → Vol froxel accum) ----------------------
    // Lazily built on the first SplatMedia call (Vol::Execute records it in the
    // World pass). One set: pool/counters/aliveList + Vol's accum SSBO; the set
    // is rewritten only when the accum buffer changes (Vol re-init — idle).
    VkDescriptorSetLayout s_mediaSetL   = VK_NULL_HANDLE;
    VkPipelineLayout      s_mediaLayout = VK_NULL_HANDLE;
    VkPipeline            s_mediaPipe   = VK_NULL_HANDLE;
    VkDescriptorPool      s_mediaPool   = VK_NULL_HANDLE;
    VkDescriptorSet       s_mediaSet    = VK_NULL_HANDLE;
    VkBuffer              s_mediaAccum  = VK_NULL_HANDLE;   // currently bound accum buffer
    bool                  s_mediaFailed = false;            // sticky until Destroy
    bool                  s_mediaRecorded = false;          // splat read pool this frame → WAR barrier before sim

    // ---- Helpers -----------------------------------------------------------
    VkShaderModule LoadShader(const char* name)
    {
        if (!g_ShaderManager) return VK_NULL_HANDLE;
        VkShaderModule m = g_ShaderManager->Load(name);
        if (m == VK_NULL_HANDLE) Msg("![VK GP] shader '%s' not found", name);
        return m;
    }

    // Shader-write → shader-read/write barrier between compute stages.
    void MemBarrier(VkCommandBuffer cmd)
    {
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    void DestroyPipelines()
    {
        auto destroy = [](VkPipeline& p) { if (p) { vkDestroyPipeline(VulkanHW.m_Device, p, nullptr); p = VK_NULL_HANDLE; } };
        destroy(s_pipeInit);
        destroy(s_pipeReset);
        destroy(s_pipeEmit);
        destroy(s_pipeSim);
        destroy(s_pipeBuild);
        destroy(s_pipeDraw);
        destroy(s_pipeDrawAdd);
        destroy(s_pipeSortHist);
        destroy(s_pipeSortScan);
        destroy(s_pipeSortScatter);
        if (s_compLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_compLayout, nullptr); s_compLayout = VK_NULL_HANDLE; }
        if (s_drawLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_drawLayout, nullptr); s_drawLayout = VK_NULL_HANDLE; }
        if (s_poolDesc)   { vkDestroyDescriptorPool(VulkanHW.m_Device, s_poolDesc, nullptr);   s_poolDesc   = VK_NULL_HANDLE; }
        if (s_setLayout)  { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
        for (u32 i = 0; i < kFramesInFlight; ++i) s_set[i] = VK_NULL_HANDLE;
        if (s_texPool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_texPool, nullptr);      s_texPool      = VK_NULL_HANDLE; }
        if (s_texSetLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_texSetLayout, nullptr); s_texSetLayout = VK_NULL_HANDLE; }
        if (s_volSetLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_volSetLayout, nullptr); s_volSetLayout = VK_NULL_HANDLE; }
        if (s_texSampler)   { vkDestroySampler(VulkanHW.m_Device, s_texSampler, nullptr);          s_texSampler   = VK_NULL_HANDLE; }
        s_texSet = VK_NULL_HANDLE; s_volSet = VK_NULL_HANDLE; s_volBoundGen = 0xFFFFFFFFu;
        destroy(s_mediaPipe);
        if (s_mediaLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_mediaLayout, nullptr); s_mediaLayout = VK_NULL_HANDLE; }
        if (s_mediaPool)   { vkDestroyDescriptorPool(VulkanHW.m_Device, s_mediaPool, nullptr);   s_mediaPool   = VK_NULL_HANDLE; }
        if (s_mediaSetL)   { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_mediaSetL, nullptr); s_mediaSetL = VK_NULL_HANDLE; }
        s_mediaSet = VK_NULL_HANDLE; s_mediaAccum = VK_NULL_HANDLE; s_mediaFailed = false; s_mediaRecorded = false;
    }

    void DestroyBuffers()
    {
        auto del = [](CVulkanBuffer*& b) { if (b) { b->Destroy(); xr_delete(b); b = nullptr; } };
        del(s_pool);
        del(s_counters);
        del(s_aliveList);
        del(s_indirect);
        del(s_freeList);
        del(s_program);
        del(s_texInfo);
        del(s_progAlive);
        del(s_sort);
        for (u32 i = 0; i < kFramesInFlight; ++i) del(s_spawnReq[i]);
        for (u32 i = 0; i < kFramesInFlight; ++i) del(s_killReq[i]);
    }

    // ---- Authored test effect (campfire smoke) -----------------------------
    // Phase 2 proof: a real action program (not hardcoded sim math) drives the
    // GPU effect. The future .pe translator fills this same Program struct.
    void FillDomainLine(GenDomain& d, float ax, float ay, float az, float bx, float by, float bz)
    {
        d = {};
        d.dtype = 1;                              // PDLine: p1 + p2*t, p2 = delta
        d.p1[0] = ax; d.p1[1] = ay; d.p1[2] = az;
        d.p2[0] = bx - ax; d.p2[1] = by - ay; d.p2[2] = bz - az;
    }
    void FillDomainBox(GenDomain& d, float minx, float miny, float minz, float maxx, float maxy, float maxz)
    {
        d = {};
        d.dtype = 4;                              // PDBox: p1=min, p2=max
        d.p1[0] = minx; d.p1[1] = miny; d.p1[2] = minz;
        d.p2[0] = maxx; d.p2[1] = maxy; d.p2[2] = maxz;
    }
    void FillDomainSphere(GenDomain& d, float cx, float cy, float cz, float r1, float r2)
    {
        d = {};
        d.dtype = 5;                              // PDSphere shell [r2..r1]
        d.p1[0] = cx; d.p1[1] = cy; d.p1[2] = cz;
        d.radii[0] = r1; d.radii[1] = r2;
        d.radii[2] = r1 * r1; d.radii[3] = r2 * r2;
    }

    ActionRec MakeAction(u32 type)
    {
        ActionRec a{};
        a.atype = type;
        return a;
    }

    void BuildTestProgram(Program& prog)
    {
        prog = {};
        prog.emitRate = 80.0f;                    // particles / second

        EmitDesc& e = prog.emit;
        FillDomainSphere(e.posDom, 0.f, 0.f, 0.f, 0.25f, 0.f);   // base puff
        FillDomainBox   (e.velDom, -0.15f, 0.6f, -0.15f, 0.15f, 1.4f, 0.15f); // rises
        FillDomainLine  (e.sizeDom, 0.18f, 0.18f, 0.f, 0.30f, 0.30f, 0.f);     // 0.18..0.30
        FillDomainBox   (e.colorDom, 0.45f, 0.45f, 0.45f, 0.65f, 0.65f, 0.65f);// grey
        FillDomainLine  (e.rotDom, 0.f, 0.f, 0.f, 6.2831853f, 0.f, 0.f);       // 0..2pi
        e.sc[0] = 0.85f;  // alpha
        e.sc[1] = 0.0f;   // initial age
        e.sc[2] = 0.0f;   // age_sigma
        e.sc[3] = 4.5f;   // lifetime

        u32 n = 0;
        // Gravity — gentle upward buoyancy.
        { ActionRec a = MakeAction(/*GP_GRAVITY*/8); a.a[1] = 0.35f; prog.actions[n++] = a; }
        // Damping — bleed ~30%/s of velocity (whole speed band).
        { ActionRec a = MakeAction(/*GP_DAMPING*/4);
          a.a[0] = a.a[1] = a.a[2] = 0.70f;       // damping
          a.b[0] = 0.0f; a.b[1] = 1.0e16f;        // vlowSqr..vhighSqr
          prog.actions[n++] = a; }
        // TargetSize — grow toward 0.8.
        { ActionRec a = MakeAction(/*GP_TARGETSIZE*/24);
          a.a[0] = a.a[1] = 0.80f;                // target
          a.b[0] = a.b[1] = 0.50f;                // scale/axis
          prog.actions[n++] = a; }
        // TargetColor — fade to dark + transparent over the whole life.
        { ActionRec a = MakeAction(/*GP_TARGETCOLOR*/23);
          a.a[0] = a.a[1] = a.a[2] = 0.20f; a.a[3] = 0.0f;   // colour.rgb, alpha
          a.b[0] = 0.50f; a.b[1] = 0.0f; a.b[2] = 1.0f;       // scale, timeFrom, timeTo
          prog.actions[n++] = a; }
        // Move — integrate position + age.
        { prog.actions[n++] = MakeAction(/*GP_MOVE*/12); }
        // KillOld — kill at lifetime.
        { ActionRec a = MakeAction(/*GP_KILLOLD*/10); a.a[0] = 4.5f; a.a[1] = 0.0f; prog.actions[n++] = a; }

        prog.actionCount = n;
    }

    // Load a program's sprite texture into its bindless slot (append-only) and
    // upload its TexInfo (atlas frame metadata). idx == program registry slot.
    void FillProgramTexture(u32 idx, const TexDesc& td)
    {
        TexInfo ti{};
        ti.layer        = -1;                               // untextured → procedural circle
        ti.flags        = td.flags;
        ti.frameSize[0] = (td.flags & 1u) ? td.frameW : 1.0f;
        ti.frameSize[1] = (td.flags & 1u) ? td.frameH : 1.0f;
        ti.frameDimX    = td.frameDimX  ? td.frameDimX  : 1u;
        ti.frameCount   = td.frameCount ? td.frameCount : 1u;
        ti.frameSpeed   = td.frameSpeed;
        ti.alignDir[0]  = td.alignDir[0];                   // bit3: zero-velocity billboard axis
        ti.alignDir[1]  = td.alignDir[1];
        ti.alignDir[2]  = td.alignDir[2];

        if (s_useTextures && td.name[0]) {
            VkImageView view = VK::ParticlePass::GetTextureView(td.name);
            if (view != VK_NULL_HANDLE) {
                // Binding 0 is the bindless sprite table — write slot `idx` of it.
                VK::DescriptorWriter(s_texSet)
                    .Image(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                              { s_texSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, idx)
                    .Flush();
                ti.layer = (s32)idx;
            } else {
                Msg("~[VK GP] program #%u texture '%s' not loaded — procedural fallback", idx, td.name);
            }
        }

        if (s_texInfo)
            s_texInfo->Upload(&ti, sizeof(ti), VkDeviceSize(idx) * sizeof(TexInfo));
    }
}

// ==========================================================================
// Init
// ==========================================================================
bool Init()
{
    VK::Vram::Scope _vram_scope("GPUParticles");
    if (s_inited) return !s_failed;
    s_inited = true;

    if (VulkanHW.m_Device == VK_NULL_HANDLE) { s_failed = true; return false; }

    s_maxP = ps_r_gpu_particles_max > 0 ? (u32)ps_r_gpu_particles_max : kDefaultMaxParticles;
    const VkDeviceSize poolSize  = VkDeviceSize(s_maxP) * sizeof(Particle);
    const VkDeviceSize idxSize   = VkDeviceSize(s_maxP) * sizeof(u32);

    // ---- Buffers -----------------------------------------------------------
    s_pool = xr_new<CVulkanBuffer>();
    s_pool->Create(poolSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    if (!s_pool->IsValid()) { Msg("![VK GP] pool alloc failed"); s_failed = true; return false; }
    Prof::NameBuffer(s_pool->GetHandle(), "GP_Pool");

    s_counters = xr_new<CVulkanBuffer>();
    s_counters->Create(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    if (!s_counters->IsValid()) { Msg("![VK GP] counters alloc failed"); s_failed = true; return false; }
    Prof::NameBuffer(s_counters->GetHandle(), "GP_Counters");

    // aliveList holds 2*maxP: the world region [0,maxP) and the HUD region
    // [maxP,2maxP). Within each, alpha grows from the front, additive from the
    // back (4 draw groups total: world/HUD × alpha/additive — see gp_simulate).
    s_aliveList = xr_new<CVulkanBuffer>();
    s_aliveList->Create(idxSize * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    if (!s_aliveList->IsValid()) { Msg("![VK GP] aliveList alloc failed"); s_failed = true; return false; }
    Prof::NameBuffer(s_aliveList->GetHandle(), "GP_AliveList");

    s_indirect = xr_new<CVulkanBuffer>();
    s_indirect->Create(64,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    if (!s_indirect->IsValid()) { Msg("![VK GP] indirect alloc failed"); s_failed = true; return false; }
    Prof::NameBuffer(s_indirect->GetHandle(), "GP_Indirect");

    s_freeList = xr_new<CVulkanBuffer>();
    s_freeList->Create(idxSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    if (!s_freeList->IsValid()) { Msg("![VK GP] freeList alloc failed"); s_failed = true; return false; }
    Prof::NameBuffer(s_freeList->GetHandle(), "GP_FreeList");

    // Program registry: Program[kMaxPrograms] — host-visible, slots written
    // on demand (slot 0 = campfire now, real .pe via gp_mirror).
    s_program = xr_new<CVulkanBuffer>();
    s_program->Create(VkDeviceSize(kMaxPrograms) * sizeof(Program),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    if (!s_program->IsValid()) { Msg("![VK GP] program alloc failed"); s_failed = true; return false; }
    Prof::NameBuffer(s_program->GetHandle(), "GP_Program");
    {
        Program prog;
        BuildTestProgram(prog);
        s_program->Upload(&prog, sizeof(prog), 0);   // slot 0
        s_progName[0]   = "<campfire>";
        s_progRate[0]   = prog.emitRate;
        s_progMaxP[0]   = 512;   // authored test effect: rate 80/s × life 4.5s ≈ 360 steady-state
        s_progCount     = 1;
        Msg("[VK GPUParticles] program #0 (campfire): %u actions, emitRate %.0f/s, life %.1fs (registry cap %u)",
            prog.actionCount, prog.emitRate, prog.emit.sc[3], kMaxPrograms);
    }

    // Camera-pinned emitter [0] — OFF by default (it would dump smoke on the
    // player and hide everything). `gp_mirror <effect>` arms it for testing;
    // real world emitters (Phase 3 #3) drive themselves regardless.
    s_emitters.clear();
    { Emitter cam; cam.program = 0; cam.followCamera = true; cam.enabled = false; s_emitters.push_back(cam); }

    // Per-frame spawn-request buffers (host-visible, one per frame in flight so
    // the CPU never overwrites a buffer the GPU is still reading).
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        s_spawnReq[i] = xr_new<CVulkanBuffer>();
        s_spawnReq[i]->Create(VkDeviceSize(kMaxEmitters) * sizeof(SpawnRequest),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        if (!s_spawnReq[i]->IsValid()) { Msg("![VK GP] spawnReq alloc failed"); s_failed = true; return false; }
        Prof::NameBuffer(s_spawnReq[i]->GetHandle(), "GP_SpawnReq");
    }

    // Per-frame emitter kill requests (hard stops / destroyed objects).
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        s_killReq[i] = xr_new<CVulkanBuffer>();
        s_killReq[i]->Create(VkDeviceSize(kMaxKills) * sizeof(KillRequest),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        if (!s_killReq[i]->IsValid()) { Msg("![VK GP] killReq alloc failed"); s_failed = true; return false; }
        Prof::NameBuffer(s_killReq[i]->GetHandle(), "GP_KillReq");
    }

    // Per-program TexInfo (atlas frame metadata, read by the billboard VS).
    s_texInfo = xr_new<CVulkanBuffer>();
    s_texInfo->Create(VkDeviceSize(kMaxPrograms) * sizeof(TexInfo),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    if (!s_texInfo->IsValid()) { Msg("![VK GP] texInfo alloc failed"); s_failed = true; return false; }
    Prof::NameBuffer(s_texInfo->GetHandle(), "GP_TexInfo");

    // #5: per-program alive counts — GPU-written (emit claims, sim releases),
    // zeroed by gp_init. Enforces cap = m_MaxParticles × live instances so a
    // greedy effect (area fog) can't starve the shared pool.
    // Host-visible so `gp_stats` can peek at the live counters (256 B, GPU
    // atomics are memory-type-agnostic on desktop; the CPU read is diagnostic
    // — a frame or two stale is fine).
    s_progAlive = xr_new<CVulkanBuffer>();
    s_progAlive->Create(VkDeviceSize(kMaxPrograms) * sizeof(u32),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    if (!s_progAlive->IsValid()) { Msg("![VK GP] progAlive alloc failed"); s_failed = true; return false; }
    Prof::NameBuffer(s_progAlive->GetHandle(), "GP_ProgAlive");

    // Phase 4 alpha-sort scratch: histogram (gp_reset zeroes it) + an unsorted
    // copy of the world-alpha aliveList region for the scatter pass.
    s_sort = xr_new<CVulkanBuffer>();
    s_sort->Create(VkDeviceSize(kSortBuckets + s_maxP) * sizeof(u32),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    if (!s_sort->IsValid()) { Msg("![VK GP] sort scratch alloc failed"); s_failed = true; return false; }
    Prof::NameBuffer(s_sort->GetHandle(), "GP_Sort");

    // ---- Bindless sprite texture array (only if descriptor indexing exists) -
    s_useTextures = VulkanHW.m_bBindlessSupported;
    if (s_useTextures) {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_texSampler) != VK_SUCCESS)
        { Msg("![VK GP] tex sampler failed"); s_useTextures = false; }
    }
    if (s_useTextures) {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = kMaxPrograms;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorBindingFlags bf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bfci.bindingCount  = 1;
        bfci.pBindingFlags = &bf;

        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.pNext        = &bfci;
        slci.bindingCount = 1;
        slci.pBindings    = &b;
        if (vkCreateDescriptorSetLayout(VulkanHW.m_Device, &slci, nullptr, &s_texSetLayout) != VK_SUCCESS)
        { Msg("![VK GP] tex set layout failed"); s_useTextures = false; }

        if (s_useTextures) {
            // Room for the bindless sprite array (set 1) + the froxel probe set (set 2).
            VkDescriptorPoolSize tps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxPrograms + 1 };
            VkDescriptorPoolCreateInfo tpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            tpci.maxSets       = 2;
            tpci.poolSizeCount = 1;
            tpci.pPoolSizes    = &tps;
            if (vkCreateDescriptorPool(VulkanHW.m_Device, &tpci, nullptr, &s_texPool) != VK_SUCCESS)
            { Msg("![VK GP] tex pool failed"); s_useTextures = false; }
        }
        if (s_useTextures) {
            VkDescriptorSetAllocateInfo dai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dai.descriptorPool     = s_texPool;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts        = &s_texSetLayout;
            if (vkAllocateDescriptorSets(VulkanHW.m_Device, &dai, &s_texSet) != VK_SUCCESS)
            { Msg("![VK GP] tex set alloc failed"); s_useTextures = false; }
        }
        // Froxel probe set (set 2 = one sampler3D, fragment stage).
        if (s_useTextures) {
            s_volSetLayout = VK::MakeSetLayout({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
                                               VK_SHADER_STAGE_FRAGMENT_BIT, "GP.Vol");
            if (s_volSetLayout == VK_NULL_HANDLE) s_useTextures = false;
        }
        if (s_useTextures) {
            if (!VK::AllocSets(s_texPool, s_volSetLayout, 1, &s_volSet, "GP.Vol"))
                s_useTextures = false;
        }
    }
    Msg("[VK GPUParticles] sprite textures: %s", s_useTextures ? "BINDLESS" : "procedural (no descriptor indexing)");

    // Campfire (slot 0) is untextured → procedural circle.
    { TexDesc td{}; FillProgramTexture(0, td); }

    // ---- Descriptor set layout (5 SSBO bindings) ---------------------------
    VkDescriptorSetLayoutBinding bindings[kNumBindings]{};
    for (u32 i = 0; i < kNumBindings; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    // pool(0) + aliveList(2) + texInfo(7) are also read by the billboard VS.
    bindings[0].stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
    bindings[2].stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
    bindings[7].stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    slci.bindingCount = kNumBindings;
    slci.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(VulkanHW.m_Device, &slci, nullptr, &s_setLayout) != VK_SUCCESS)
    { Msg("![VK GP] set layout failed"); s_failed = true; return false; }

    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight * kNumBindings };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets       = kFramesInFlight;
    pci.poolSizeCount = 1;
    pci.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(VulkanHW.m_Device, &pci, nullptr, &s_poolDesc) != VK_SUCCESS)
    { Msg("![VK GP] desc pool failed"); s_failed = true; return false; }

    {
        VkDescriptorSetLayout layouts[kFramesInFlight];
        for (u32 i = 0; i < kFramesInFlight; ++i) layouts[i] = s_setLayout;
        VkDescriptorSetAllocateInfo dai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dai.descriptorPool     = s_poolDesc;
        dai.descriptorSetCount = kFramesInFlight;
        dai.pSetLayouts        = layouts;
        if (vkAllocateDescriptorSets(VulkanHW.m_Device, &dai, s_set) != VK_SUCCESS)
        { Msg("![VK GP] set alloc failed"); s_failed = true; return false; }
    }

    // ---- Pipeline layouts --------------------------------------------------
    {
        VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush) };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount         = 1;
        plci.pSetLayouts            = &s_setLayout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_compLayout) != VK_SUCCESS)
        { Msg("![VK GP] comp layout failed"); s_failed = true; return false; }
    }
    {
        // Push is VERTEX-only on the procedural path; on the bindless path the
        // fragment also reads it (froxel probe strength/clamp). set2 = froxel probe.
        VkPushConstantRange pcr{
            VkShaderStageFlags(s_useTextures ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                                             : VK_SHADER_STAGE_VERTEX_BIT), 0, sizeof(DrawPush) };
        VkDescriptorSetLayout sets[3] = { s_setLayout, s_texSetLayout, s_volSetLayout };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount         = s_useTextures ? 3u : 1u;  // set1 = sprites, set2 = froxel probe
        plci.pSetLayouts            = sets;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_drawLayout) != VK_SUCCESS)
        { Msg("![VK GP] draw layout failed"); s_failed = true; return false; }
    }

    // ---- Shaders -----------------------------------------------------------
    VkShaderModule modInit  = LoadShader("gp_init.comp.spv");
    VkShaderModule modReset = LoadShader("gp_reset.comp.spv");
    VkShaderModule modEmit  = LoadShader("gp_emit.comp.spv");
    VkShaderModule modSim   = LoadShader("gp_simulate.comp.spv");
    VkShaderModule modBuild = LoadShader("gp_build.comp.spv");
    VkShaderModule modSortH = LoadShader("gp_sort_hist.comp.spv");
    VkShaderModule modSortS = LoadShader("gp_sort_scan.comp.spv");
    VkShaderModule modSortC = LoadShader("gp_sort_scatter.comp.spv");
    VkShaderModule modVS    = LoadShader("gp_particle.vert.spv");
    VkShaderModule modFS    = LoadShader(s_useTextures ? "gp_particle_tex.frag.spv" : "gp_particle.frag.spv");
    if (!modInit || !modReset || !modEmit || !modSim || !modBuild || !modSortH || !modSortS || !modSortC || !modVS || !modFS)
    { s_failed = true; return false; }

    s_pipeInit  = VK::CreateComputePipeline(modInit,  s_compLayout, "GP.Init");
    s_pipeReset = VK::CreateComputePipeline(modReset, s_compLayout, "GP.Reset");
    s_pipeEmit  = VK::CreateComputePipeline(modEmit,  s_compLayout, "GP.Emit");
    s_pipeSim   = VK::CreateComputePipeline(modSim,   s_compLayout, "GP.Simulate");
    s_pipeBuild = VK::CreateComputePipeline(modBuild, s_compLayout, "GP.Build");
    s_pipeSortHist    = VK::CreateComputePipeline(modSortH, s_compLayout, "GP.Sort.Hist");
    s_pipeSortScan    = VK::CreateComputePipeline(modSortS, s_compLayout, "GP.Sort.Scan");
    s_pipeSortScatter = VK::CreateComputePipeline(modSortC, s_compLayout, "GP.Sort.Scatter");
    if (!s_pipeInit || !s_pipeReset || !s_pipeEmit || !s_pipeSim || !s_pipeBuild ||
        !s_pipeSortHist || !s_pipeSortScan || !s_pipeSortScatter)
    { Msg("![VK GP] compute pipeline creation failed"); s_failed = true; return false; }

    // ---- Graphics pipeline (additive-free alpha billboards) ----------------
    {
        // Procedural billboards (no vertex input), depth-tested but not written.
        // The additive variant (fire/sparks/muzzle) is the same pipeline with a
        // different destination colour factor; additive is order-independent, so
        // those need no sort.
        auto makeDrawPipe = [&](bool additive) {
            VK::GfxPipelineBuilder b(s_drawLayout);
            b.Vert(modVS).Frag(modFS)
             .Depth(true, false)
             .Color(VK::SceneColor::Format());
            (additive ? b.BlendAdd() : b.BlendAlpha());
            return b.DepthTarget(Swapchain.m_DepthFormat)
                    .Build("GP draw%s", additive ? " additive" : "");
        };
        s_pipeDraw    = makeDrawPipe(false);
        s_pipeDrawAdd = makeDrawPipe(true);
        if (!s_pipeDraw || !s_pipeDrawAdd) { s_failed = true; return false; }
    }

    // ---- Descriptor writes -------------------------------------------------
    {
        for (u32 i = 0; i < kFramesInFlight; ++i) {
            // binding 6 (spawn requests) is per-frame; the rest are shared.
            VkDescriptorBufferInfo bi[kNumBindings] = {
                { s_pool->GetHandle(),       0, VK_WHOLE_SIZE },
                { s_counters->GetHandle(),   0, VK_WHOLE_SIZE },
                { s_aliveList->GetHandle(),  0, VK_WHOLE_SIZE },
                { s_indirect->GetHandle(),   0, VK_WHOLE_SIZE },
                { s_freeList->GetHandle(),   0, VK_WHOLE_SIZE },
                { s_program->GetHandle(),    0, VK_WHOLE_SIZE },
                { s_spawnReq[i]->GetHandle(),0, VK_WHOLE_SIZE },
                { s_texInfo->GetHandle(),    0, VK_WHOLE_SIZE },
                { s_progAlive->GetHandle(),  0, VK_WHOLE_SIZE },
                { s_sort->GetHandle(),       0, VK_WHOLE_SIZE },
                { s_killReq[i]->GetHandle(), 0, VK_WHOLE_SIZE },
            };
            VK::DescriptorWriter w(s_set[i]);
            for (u32 b = 0; b < kNumBindings; ++b)
                w.Buffer(b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bi[b]);
            w.Flush();
        }
    }

    s_poolInited = false;   // gp_init runs lazily on the first dispatch
    Msg("[VK GPUParticles] init OK — %u particles (%llu MB pool), r_gpu_particles %d",
        s_maxP, (unsigned long long)(poolSize >> 20), ps_r_gpu_particles);
    return true;
}

// ==========================================================================
// Program registry — resolve an effect name to a slot, translating on demand.
// Returns the slot index, or -1 on failure. Empty/null name → slot 0 (campfire).
// Public (the world-emitter feed in vk_gpu_particles_feed.cpp calls it).
// ==========================================================================
int ResolveProgram(const char* name)
{
    if (!s_inited || s_failed || !s_program) return -1;
    if (!name || !name[0]) return 0;             // campfire

    // Already registered? Reuse the slot (no re-translate/upload).
    for (u32 i = 1; i < s_progCount; ++i)
        if (s_progName[i].size() && 0 == strcmp(s_progName[i].c_str(), name))
            return (int)i;

    if (s_progCount >= kMaxPrograms) {
        Msg("![VK GP] registry full (%u programs) — can't add '%s'", kMaxPrograms, name);
        return -1;
    }

    Program prog;
    float   rate = 0.0f;
    TexDesc tex{};
    u32     maxP = 0;
    if (!TranslateEffect(name, prog, rate, tex, maxP))  // logs its own failure reason
        return -1;

    const u32 idx = s_progCount++;
    s_progName[idx] = name;
    s_progRate[idx] = rate;
    s_progLife[idx] = prog.emit.sc[3];            // lifetime (sc.w) for fallback budget gating
    s_progMaxP[idx] = maxP;                       // per-instance cap (#5) — 0 = def had none
    s_program->Upload(&prog, sizeof(prog), VkDeviceSize(idx) * sizeof(Program));
    FillProgramTexture(idx, tex);                 // bindless slot + atlas TexInfo
    Msg("[VK GPUParticles] registered '%s' as program #%u (emitRate %.0f/s, life %.1fs, maxP %u, tex '%s')",
        name, idx, rate, s_progLife[idx], maxP, tex.name[0] ? tex.name : "<none>");
    return (int)idx;
}

bool  Enabled()              { return s_inited && !s_failed && ps_r_gpu_particles != 0; }
float ProgramRate(int slot)  { return (slot >= 0 && (u32)slot < s_progCount) ? s_progRate[slot] : 0.0f; }
float ProgramLife(int slot)  { return (slot >= 0 && (u32)slot < s_progCount) ? s_progLife[slot] : 0.0f; }
u32   ProgramMaxP(int slot)  { return (slot >= 0 && (u32)slot < s_progCount) ? s_progMaxP[slot] : 0u; }

// Safe to auto-route onto the shared GPU pool? With per-program alive caps
// (#5: cap = m_MaxParticles × live instances, enforced in gp_emit) any effect
// whose PER-INSTANCE budget fits a pool share routes — including area fog /
// persistent fields the old rate×life gate refused. The rate×life budget
// remains only as a fallback for defs without an authored m_MaxParticles.
bool ProgramRoutable(int slot)
{
    if (slot < 0 || (u32)slot >= s_progCount) return false;
    if (s_progMaxP[slot]) return s_progMaxP[slot] <= s_maxP / 4;
    return s_progRate[slot] * s_progLife[slot] <= kRouteBudget;
}

// #5 live-instance registry — the cap basis. Counting only frustum-visible
// emitters made the flame budget wander between campfires (one immortal
// billboard shared by the whole camp); live OBJECTS mirror the CPU's
// per-instance pool lifetime exactly.
void AddProgramInstance(int slot)
{
    if (slot >= 0 && (u32)slot < kMaxPrograms) ++s_progInstances[slot];
}
void ReleaseProgramInstance(int slot)
{
    if (slot < 0 || (u32)slot >= kMaxPrograms) return;
    // Deferred: the instance's ghost particles keep using its budget for up to
    // one particle lifetime — release the cap share only once they're gone.
    const float life = ((u32)slot < s_progCount) ? _max(s_progLife[slot], 1.0f) : 1.0f;
    std::lock_guard<std::mutex> g(s_releaseMx);
    s_pendingRelease.push_back({ slot, Device.fTimeGlobal + life });
}

// CPU parity for hard stops / destruction — see the header comment.
void QueueKill(int slot, const Fvector& pos, float radius)
{
    if (slot < 0 || (u32)slot >= kMaxPrograms || !s_inited || s_failed) return;
    KillRequest k{};
    k.posRadius[0] = pos.x; k.posRadius[1] = pos.y; k.posRadius[2] = pos.z;
    k.posRadius[3] = radius * radius;
    k.program      = (u32)slot;
    std::lock_guard<std::mutex> g(s_killMx);
    s_pendingKills.push_back(k);
}

// `gp_stats` — per-program budget diagnostics: authored per-instance maxP,
// live claimed objects (cap basis) and the GPU's alive counter (host-visible
// peek, a frame stale). Nails "why doesn't X spawn" straight from the console.
void DumpStats()
{
    if (!s_inited || s_failed) { Msg("![VK GP] gp_stats: module not initialised"); return; }
    const u32* alive = s_progAlive ? (const u32*)s_progAlive->Map() : nullptr;
    Msg("[VK GP] gp_stats: %u program(s), pool %u", s_progCount, s_maxP);
    for (u32 i = 0; i < s_progCount; ++i) {
        const s32 inst = s_progInstances[i].load(std::memory_order_relaxed);
        Msg("  #%u '%s': maxP %u x inst %d = cap %u | alive(GPU) %u | rate %.0f/s life %.1fs",
            i, s_progName[i].c_str(), s_progMaxP[i], inst,
            s_progMaxP[i] * u32(_max(inst, s32(0))),
            alive ? alive[i] : 0u, s_progRate[i], s_progLife[i]);
    }
}

// ==========================================================================
// MirrorEffect — retarget the camera-pinned emitter (console gp_mirror)
// ==========================================================================
bool MirrorEffect(const char* name)
{
    if (!s_inited || s_failed || !s_program || s_emitters.empty()) {
        Msg("![VK GP] gp_mirror: GPU particles not initialised — set r_gpu_particles 1 first");
        return false;
    }

    // No arg → turn the camera test emitter OFF (don't dump smoke on the player).
    if (!name || !name[0]) {
        s_emitters[0].enabled = false;
        s_emitters[0].accum   = 0.0f;
        Msg("[VK GPUParticles] gp_mirror: camera emitter OFF (no effect arg)");
        return true;
    }

    const int slot = ResolveProgram(name);
    if (slot < 0) return false;

    s_emitters[0].program = (u32)slot;
    s_emitters[0].enabled = true;
    s_emitters[0].accum   = 0.0f;
    Msg("[VK GPUParticles] gp_mirror: camera emitter -> program #%d (%s), ON",
        slot, name);
    return true;
}

// ==========================================================================
// SpawnEffect / ClearSpawns — persistent world emitters (console gp_spawn)
// ==========================================================================
bool SpawnEffect(const char* name)
{
    if (!s_inited || s_failed || !s_program) {
        Msg("![VK GP] gp_spawn: GPU particles not initialised — set r_gpu_particles 1 first");
        return false;
    }
    if (!name || !name[0]) {
        Msg("![VK GP] gp_spawn: usage 'gp_spawn <effect>' (see gp_list)");
        return false;
    }
    if (s_emitters.size() >= kMaxEmitters) {
        Msg("![VK GP] gp_spawn: emitter cap (%u) reached — gp_spawn_clear first", kMaxEmitters);
        return false;
    }

    const int slot = ResolveProgram(name);
    if (slot < 0) return false;

    Emitter em;
    em.program      = (u32)slot;
    em.followCamera = false;
    em.pos          = Device.vCameraPosition;
    em.pos.y       -= 0.5f;
    s_emitters.push_back(em);
    Msg("[VK GPUParticles] gp_spawn: '%s' (program #%d) at (%.1f %.1f %.1f) — %u world emitter(s)",
        name, slot, em.pos.x, em.pos.y, em.pos.z, (u32)s_emitters.size() - 1);
    return true;
}

void ClearSpawns()
{
    if (s_emitters.size() > 1) s_emitters.resize(1);   // keep the camera emitter
    Msg("[VK GPUParticles] gp_spawn_clear: world emitters removed");
}

// ==========================================================================
// #6 media splat — GPU smoke feeds the froxel fog without CPU particles.
// Vol::Execute records this between its accum clear and resolve (World pass,
// before this frame's gp_reset/sim → reads LAST frame's alive list).
// ==========================================================================
bool WantsMediaSplat()
{
    return s_inited && !s_failed && !s_mediaFailed && ps_r_gpu_particles != 0 && s_poolInited;
}

bool SplatMedia(VkCommandBuffer cmd, VkBuffer accumBuf, const MediaSplatPush& push)
{
    if (!WantsMediaSplat() || cmd == VK_NULL_HANDLE || accumBuf == VK_NULL_HANDLE) return false;

    // Lazy pipeline / set (first Vol frame with the GP pool live).
    if (s_mediaPipe == VK_NULL_HANDLE) {
        auto fail = [&](const char* what) { Msg("![VK GP] media splat %s failed", what); s_mediaFailed = true; return false; };

        constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (!VK::MakeDescriptorSets({ kSSBO, kSSBO, kSSBO, kSSBO }, 1,
                                    s_mediaSetL, s_mediaPool, &s_mediaSet,
                                    VK_SHADER_STAGE_COMPUTE_BIT, "GP.MediaSplat"))
            return fail("descriptors");

        s_mediaLayout = VK::MakePipelineLayout({ s_mediaSetL }, sizeof(MediaSplatPush));
        if (s_mediaLayout == VK_NULL_HANDLE) return fail("pipeline layout");

        s_mediaPipe = VK::CreateComputePipeline("gp_media_splat.comp.spv", s_mediaLayout, "GP.MediaSplat");
        if (s_mediaPipe == VK_NULL_HANDLE) return fail("pipeline");

        Msg("[VK GP] media splat ready — GPU smoke feeds the froxel fog directly");
    }

    if (accumBuf != s_mediaAccum) {
        VkDescriptorBufferInfo bi[4] = {
            { s_pool->GetHandle(),      0, VK_WHOLE_SIZE },
            { s_counters->GetHandle(),  0, VK_WHOLE_SIZE },
            { s_aliveList->GetHandle(), 0, VK_WHOLE_SIZE },
            { accumBuf,                 0, VK_WHOLE_SIZE },
        };
        VK::DescriptorWriter w(s_mediaSet);
        for (u32 i = 0; i < 4; ++i) w.Buffer(i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bi[i]);
        w.Flush();
        s_mediaAccum = accumBuf;
    }

    const int z = Prof::ZoneBegin(cmd, "GP::MediaSplat");
    MediaSplatPush mp = push;
    mp.params[0] = float(s_maxP);   // aliveList bounds guard (count read on-GPU from counters[1])
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_mediaPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_mediaLayout, 0, 1, &s_mediaSet, 0, nullptr);
    vkCmdPushConstants(cmd, s_mediaLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mp), &mp);
    vkCmdDispatch(cmd, (s_maxP + 63u) / 64u, 1, 1);
    Prof::ZoneEnd(cmd, z);

    s_mediaRecorded = true;
    return true;
}

// ==========================================================================
// DispatchComputeAndDraw — init(once) → reset → emit → sim → build → draw
// ==========================================================================
// Two-slot pipeline: the sample list is per frame slot (extract of frame N+1 fills its own, record N reads its).
struct WorldSamplesSlot { xr_vector<EmitterSample> v; u32 tag = u32(-1); };
static RT::PerSlot<WorldSamplesSlot> s_worldSlots;

void ExtractWorldEmitters()
{
    WorldSamplesSlot& ws = s_worldSlots.cur();
    ws.v.clear();
    ws.tag = RV().frameIndex;
    if (!s_inited || s_failed || ps_r_gpu_particles == 0) return;   // same gate as DispatchComputeAndDraw
    VK_GP_CollectWorldEmitters(ws.v, RV().fTimeDelta);
}

void DispatchComputeAndDraw(FrameContext& ctx)
{
    if (!s_inited || s_failed || ps_r_gpu_particles == 0) return;
    if (ctx.cmd == VK_NULL_HANDLE) return;

    const u32 slot = CommandManager.GetCurrentFrame() % kFramesInFlight;
    VkCommandBuffer cmd = ctx.cmd;
    const u32 maxP = s_maxP;

    // #6: the World pass's media splat READ pool/aliveList/counters earlier in
    // this command buffer; order those reads before this pass's writes (WAR —
    // the compute→compute execution dependency is what matters).
    if (s_mediaRecorded) { MemBarrier(cmd); s_mediaRecorded = false; }

    auto bindCompute = [&](VkPipeline pipe) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_compLayout, 0, 1, &s_set[slot], 0, nullptr);
    };

    // ---- Drain matured deferred instance releases (#5 budget hygiene) -----
    {
        std::lock_guard<std::mutex> g(s_releaseMx);
        for (size_t i = 0; i < s_pendingRelease.size();) {
            if (RV().fTimeGlobal >= s_pendingRelease[i].when) {
                const int sl = s_pendingRelease[i].slot;
                if (sl >= 0 && (u32)sl < kMaxPrograms && --s_progInstances[sl] < 0)
                    s_progInstances[sl] = 0;   // defensive
                s_pendingRelease[i] = s_pendingRelease.back();
                s_pendingRelease.pop_back();
            } else ++i;
        }
    }

    // ---- Gather emitter samples (camera + gp_spawn + real world .pe) ------
    const float dt = RV().fTimeDelta;
    static xr_vector<EmitterSample> samples;
    samples.clear();
    Fvector camBelow = RV().vCameraPosition; camBelow.y -= 0.5f;

    // (a) camera-pinned + gp_spawn world emitters (persistent accum in Emitter).
    for (Emitter& em : s_emitters) {
        if (!em.enabled) continue;
        em.accum += s_progRate[em.program] * dt;
        u32 cnt = (u32)em.accum;
        em.accum -= (float)cnt;                  // keep the fraction
        // Zero-count samples are kept: they contribute to the per-program
        // instance count (#5 alive cap) but never become spawn requests.
        EmitterSample es;
        es.program = em.program;
        es.count   = cnt;
        es.pos     = em.followCamera ? camBelow : em.pos;
        es.vel.set(0, 0, 0);
        samples.push_back(es);
    }

    // (b) real .pe emitters playing in the world -- pulled from the live effects (and it advances their
    //     spawn accumulators), so since RENDER_THREAD_PLAN step 7 it is done by GPUParticles::ExtractWorldEmitters
    //     in the PREPARE phase; here we only take the result.
    {
        const WorldSamplesSlot& ws = s_worldSlots.cur();
        if (ws.tag == RV().frameIndex)
            samples.insert(samples.end(), ws.v.begin(), ws.v.end());
    }

    // ---- Convert samples → dense spawn-request gid ranges -----------------
    // Each request claims [firstSlot, firstSlot+count); the emit shader walks
    // them to spawn the right program at the right world position.
    //
    // #5 per-program alive cap = m_MaxParticles × instances emitting THIS frame
    // (mirrors the CPU path, where each instance owned its own m_MaxParticles
    // pool), clamped so no program family owns more than half the shared pool.
    // Carried in sr.pos.w (uint bits); gp_emit claims progAlive[] against it.
    u32 instCount[kMaxPrograms] = {};
    for (const EmitterSample& es : samples)
        if (es.program < kMaxPrograms) ++instCount[es.program];

    SpawnRequest reqs[kMaxEmitters];
    u32 numReq = 0, emitCount = 0;
    for (const EmitterSample& es : samples) {
        if (emitCount >= kMaxEmitPerFrame || numReq >= kMaxEmitters) break;
        u32 cnt = es.count;
        if (cnt > kMaxEmitPerEmitter) cnt = kMaxEmitPerEmitter;
        if (emitCount + cnt > kMaxEmitPerFrame) cnt = kMaxEmitPerFrame - emitCount;
        if (cnt == 0) continue;

        u32 cap = 0;   // 0 = uncapped (def had no m_MaxParticles)
        if (es.program < kMaxPrograms && s_progMaxP[es.program]) {
            // Cap basis = live claimed OBJECTS (CPU per-instance pool parity);
            // this frame's visible-emitter count is the lower bound (covers
            // camera/gp_spawn test emitters that aren't registry-tracked).
            const s32 liveS = s_progInstances[es.program].load(std::memory_order_relaxed);
            const u32 inst  = _max(u32(_max(liveS, s32(0))), instCount[es.program]);
            const u64 c = u64(s_progMaxP[es.program]) * inst;
            const u32 capMax = maxP / 2;
            cap = (c > capMax) ? capMax : (u32)c;
        }

        SpawnRequest& sr = reqs[numReq];
        sr.pos[0] = es.pos.x; sr.pos[1] = es.pos.y; sr.pos[2] = es.pos.z;
        memcpy(&sr.pos[3], &cap, sizeof(cap));
        sr.vel[0] = es.vel.x; sr.vel[1] = es.vel.y; sr.vel[2] = es.vel.z; sr.vel[3] = 0.0f;
        // bit31 of program = HUD emitter → gp_simulate routes it to the HUD
        // aliveList region, drawn with the HUD-FOV projection + near depth.
        sr.program   = es.program | (es.hud ? 0x80000000u : 0u);
        sr.count     = cnt;
        sr.firstSlot = emitCount;
        sr.seed      = RV().frameIndex * 2654435761u + (numReq + 1u) * 2246822519u;
        emitCount += cnt;
        ++numReq;
    }
    if (numReq > 0)
        s_spawnReq[slot]->Upload(reqs, VkDeviceSize(numReq) * sizeof(SpawnRequest), 0);

    // ---- Drain queued emitter kills into this frame's buffer ---------------
    u32 numKills = 0;
    {
        std::lock_guard<std::mutex> g(s_killMx);
        if (!s_pendingKills.empty()) {
            numKills = (u32)_min((size_t)kMaxKills, s_pendingKills.size());
            s_killReq[slot]->Upload(s_pendingKills.data(), VkDeviceSize(numKills) * sizeof(KillRequest), 0);
            s_pendingKills.erase(s_pendingKills.begin(), s_pendingKills.begin() + numKills);
        }
    }

    // Shared compute push.
    ComputePush push{};
    push.spawnPos[0] = camBelow.x;               // reserved (emit reads request pos)
    push.spawnPos[1] = camBelow.y;
    push.spawnPos[2] = camBelow.z;
    memcpy(&push.spawnPos[3], &emitCount, 4);
    push.dt_gravity[0] = dt;
    push.dt_gravity[1] = 9.8f;
    push.dt_gravity[2] = RV().fTimeGlobal;     // scroll time for Turbulence noise field
    // Camera basis for the Phase 4 depth sort (view-Z bucketing).
    push.camPos[0]     = RV().vCameraPosition.x;
    push.camPos[1]     = RV().vCameraPosition.y;
    push.camPos[2]     = RV().vCameraPosition.z;
    push.camForward[0] = RV().vCameraDirection.x;
    push.camForward[1] = RV().vCameraDirection.y;
    push.camForward[2] = RV().vCameraDirection.z;
    push.maxParticles  = maxP;
    push.frameSeed     = RV().frameIndex;
    push.activeProgram = 0;                       // reserved
    push.numRequests   = numReq;
    push.numKills      = numKills;

    // ---- One-time pool init ------------------------------------------------
    if (!s_poolInited) {
        const int z = Prof::ZoneBegin(cmd, "GP::Init");
        bindCompute(s_pipeInit);
        vkCmdPushConstants(cmd, s_compLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (maxP + 63) / 64, 1, 1);
        MemBarrier(cmd);
        Prof::ZoneEnd(cmd, z);
        s_poolInited = true;
    }

    // ---- Reset alive counter ----------------------------------------------
    {
        const int z = Prof::ZoneBegin(cmd, "GP::Reset");
        bindCompute(s_pipeReset);
        vkCmdPushConstants(cmd, s_compLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, 1, 1, 1);
        Prof::ZoneEnd(cmd, z);
    }

    // ---- Emit (free-list pop) ---------------------------------------------
    if (emitCount > 0) {
        const int z = Prof::ZoneBegin(cmd, "GP::Emit");
        bindCompute(s_pipeEmit);
        vkCmdPushConstants(cmd, s_compLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (emitCount + 63) / 64, 1, 1);
        Prof::ZoneEnd(cmd, z);
    }

    MemBarrier(cmd);   // emit/reset writes → sim reads

    // ---- Simulate ----------------------------------------------------------
    {
        const int z = Prof::ZoneBegin(cmd, "GP::Sim");
        bindCompute(s_pipeSim);
        vkCmdPushConstants(cmd, s_compLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (maxP + 63) / 64, 1, 1);
        Prof::ZoneEnd(cmd, z);
    }

    MemBarrier(cmd);   // sim writes aliveCount/aliveList → sort/build read

    // ---- Phase 4: depth-sort the world-alpha region (back-to-front) --------
    // Counting sort over 512 log-Z buckets: histogram → exclusive scan →
    // scatter (rewrites aliveList[0..worldAlpha) in place; additive and HUD
    // regions untouched — additive is order-independent, HUD counts are tiny).
    if (ps_r_gpu_particles_sort) {
        const int z = Prof::ZoneBegin(cmd, "GP::Sort");
        bindCompute(s_pipeSortHist);
        vkCmdPushConstants(cmd, s_compLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (maxP + 63) / 64, 1, 1);
        MemBarrier(cmd);   // histogram + unsorted copy → scan reads
        bindCompute(s_pipeSortScan);
        vkCmdPushConstants(cmd, s_compLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, 1, 1, 1);
        MemBarrier(cmd);   // scan offsets → scatter claims
        bindCompute(s_pipeSortScatter);
        vkCmdPushConstants(cmd, s_compLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (maxP + 63) / 64, 1, 1);
        MemBarrier(cmd);   // sorted aliveList → build/draw read
        Prof::ZoneEnd(cmd, z);
    }

    // ---- Build indirect ----------------------------------------------------
    {
        const int z = Prof::ZoneBegin(cmd, "GP::Build");
        bindCompute(s_pipeBuild);
        vkCmdPushConstants(cmd, s_compLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, 1, 1, 1);

        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
        Prof::ZoneEnd(cmd, z);
    }

    // ---- Draw billboards ---------------------------------------------------
    {
        const int z = Prof::ZoneBegin(cmd, "GP::Draw");

        VkViewport vp{ 0.0f, (float)ctx.extent.height, (float)ctx.extent.width,
                       -(float)ctx.extent.height, 0.0f, 1.0f };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc{ {}, ctx.extent };
        vkCmdSetScissor(cmd, 0, 1, &sc);

        VK::RenderingBuilder rb(ctx.extent);
        rb.Color(ctx.colorView)
          .Depth(ctx.depthView, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE);

        // ---- Froxel volumetric light-probe params (Stage-0 smoke lighting) --
        // Re-point set 2 at Vol's scatter volume when it (re)generates, then
        // gather the probe params. strength 0 disables the probe (r_vol off, or
        // the additive / HUD draws). Bindless path only.
        float probeStrength = 0.0f, probeNear = 1.0f, probeLogFN = 1.0f;
        const float probeClamp = ps_r_vol_smoke_clamp;
        if (s_useTextures && s_volSet && VK::Vol::Ready()) {
            const u32 gen = VK::Vol::Generation();
            if (gen != s_volBoundGen) {
                VkImageView sv = VK::Vol::GetScatterView();
                VkSampler   ss = VK::Vol::GetSampler();
                if (sv && ss) {
                    VK::DescriptorWriter(s_volSet).ImageSampler(0, sv, ss).Flush();
                    s_volBoundGen = gen;
                }
            }
            if (s_volBoundGen == gen && VK::Vol::Wanted() && VK::Vol::ProbeAvailableToGraphics()) {
                const VK::Vol::GridZParams gz = VK::Vol::GetGridZ();
                probeNear     = gz.nearZ;
                probeLogFN    = gz.logFarNear;
                probeStrength = ps_r_vol_smoke;
            }
        }

        rb.Begin(cmd);

        DrawPush dpush{};
        static_assert(sizeof(Fmatrix) == 64, "Fmatrix 64 B");
        memcpy(dpush.viewProj, &RV().mFullTransform, 64);
        dpush.camRight[0] = RV().vCameraRight.x;
        dpush.camRight[1] = RV().vCameraRight.y;
        dpush.camRight[2] = RV().vCameraRight.z;
        dpush.camRight[3] = probeNear;
        dpush.camUp[0]    = RV().vCameraTop.x;
        dpush.camUp[1]    = RV().vCameraTop.y;
        dpush.camUp[2]    = RV().vCameraTop.z;
        dpush.camUp[3]    = probeLogFN;
        dpush.camPosStr[0] = RV().vCameraPosition.x;
        dpush.camPosStr[1] = RV().vCameraPosition.y;
        dpush.camPosStr[2] = RV().vCameraPosition.z;
        dpush.camPosStr[3] = probeStrength;
        dpush.camDirClamp[0] = RV().vCameraDirection.x;
        dpush.camDirClamp[1] = RV().vCameraDirection.y;
        dpush.camDirClamp[2] = RV().vCameraDirection.z;
        dpush.camDirClamp[3] = probeClamp;

        const VkShaderStageFlags pcStages = s_useTextures
            ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) : VK_SHADER_STAGE_VERTEX_BIT;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_drawLayout, 0, 1, &s_set[slot], 0, nullptr);
        if (s_useTextures && s_texSet)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_drawLayout, 1, 1, &s_texSet, 0, nullptr);
        if (s_useTextures && s_volSet)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_drawLayout, 2, 1, &s_volSet, 0, nullptr);

        const VkDeviceSize kCmd = sizeof(VkDrawIndirectCommand);   // 16 B

        // ---- World billboards: scene projection, full depth range ----------
        // cmds: 0=world-alpha 1=world-add 2=hud-alpha 3=hud-add. Only world
        // alpha smoke gets the froxel probe; additive is self-emissive (strength
        // 0) and HUD uses a different projection (probe would misproject).
        vkCmdPushConstants(cmd, s_drawLayout, pcStages, 0, sizeof(dpush), &dpush);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeDraw);
        vkCmdDrawIndirect(cmd, s_indirect->GetHandle(), 0 * kCmd, 1, kCmd);

        dpush.camPosStr[3] = 0.0f;   // additive: probe off
        vkCmdPushConstants(cmd, s_drawLayout, pcStages, 0, sizeof(dpush), &dpush);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeDrawAdd);
        vkCmdDrawIndirect(cmd, s_indirect->GetHandle(), 1 * kCmd, 1, kCmd);

        // ---- HUD billboards (muzzle flashes): HUD-FOV projection + near depth
        //      range [0, 0.02] so they sit on the weapon and never clip walls
        //      (mirrors the CPU HUD phase in vk_pass_particles). Probe off.
        DrawPush hpush = dpush;      // strength already 0
        memcpy(hpush.viewProj, &RV().mFullTransform_hud2, 64);
        vkCmdPushConstants(cmd, s_drawLayout, pcStages, 0, sizeof(hpush), &hpush);
        VkViewport hvp = vp; hvp.minDepth = 0.0f; hvp.maxDepth = 0.02f;
        vkCmdSetViewport(cmd, 0, 1, &hvp);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeDraw);
        vkCmdDrawIndirect(cmd, s_indirect->GetHandle(), 2 * kCmd, 1, kCmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeDrawAdd);
        vkCmdDrawIndirect(cmd, s_indirect->GetHandle(), 3 * kCmd, 1, kCmd);

        vkCmdEndRendering(cmd);
        Prof::ZoneEnd(cmd, z);
    }
}

// ==========================================================================
// Destroy
// ==========================================================================
void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    DestroyPipelines();
    DestroyBuffers();
    s_inited     = false;
    s_failed     = false;
    s_poolInited = false;
    for (u32 i = 0; i < kMaxPrograms; ++i) {
        s_progName[i] = shared_str(); s_progRate[i] = 0.0f; s_progLife[i] = 0.0f;
        s_progMaxP[i] = 0; s_progInstances[i] = 0;
    }
    s_progCount = 0;
    s_emitters.clear();
    { std::lock_guard<std::mutex> g(s_releaseMx); s_pendingRelease.clear(); }
    { std::lock_guard<std::mutex> g(s_killMx);    s_pendingKills.clear(); }
    Msg("[VK GPUParticles] destroyed");
}

}}  // namespace VK::GPUParticles
