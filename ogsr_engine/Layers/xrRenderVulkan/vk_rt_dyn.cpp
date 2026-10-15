// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// xrRenderVulkan — per-frame dynamic BLAS/TLAS over the pre-skinning pool. See vk_rt_dyn.h.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_rt_dyn.h"
#include "vk_rt_scene.h"           // RTScene::Fns / StaticInstances / Ready / Supported
#include "vk_pass_skinned.h"       // Skinned_PreSkinRtLeaves / Skinned_PreSkinPoolAddress
#include "vk_buffer.h"
#include "vk_barriers.h"           // VK::MemoryBarrier
#include "vk_profiler.h"           // VK::Prof zones
#include "vk_command_buffer.h"     // CommandManager.GetCurrentFrame()
#include "HW_Vulkan.h"
#include "../../xr_3da/device.h"   // RV().frameIndex

extern int ps_r_rt_dyn;
extern int ps_r_rtao;
extern int ps_r_preskin;

namespace VK { namespace RTDyn {

namespace {

constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;
// One geometry per leaf; a scene with more skeleton leaves than this on screen
// keeps the first ones (the list is in upload order — world skeletons first).
constexpr u32 kMaxLeaves = 2048;

struct Slot {
    CVulkanBuffer blasBuf;    // BLAS backing store (grows with hysteresis, never shrinks)
    CVulkanBuffer tlasBuf;    // TLAS backing store
    CVulkanBuffer instBuf;    // host-visible: up to 3 instances, +16 B so the address can be aligned
    CVulkanBuffer scratch;    // build scratch, one region — BLAS then TLAS, barrier between
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    VkDeviceSize blasCap = 0;  // size the BLAS object was created with
    VkDeviceSize tlasCap = 0;
    u32 tris = 0;
};
Slot s_slot[kFramesInFlight];
u32  s_cur   = ~0u;
u64  s_frame = ~0ull;
bool s_ready = false;
u32  s_gen   = 0;
u32  s_leaves = 0, s_tris = 0;
float s_blasMB = 0.f;

// Scratch lists, reused frame to frame.
xr_vector<VkAccelerationStructureGeometryKHR>       s_geo;
xr_vector<VkAccelerationStructureBuildRangeInfoKHR> s_range;
xr_vector<u32>                                      s_prim;

VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) & ~(a - 1); }

bool EnsureBuffer(CVulkanBuffer& b, VkDeviceSize need, VkBufferUsageFlags usage, bool gpuOnly)
{
    if (b.IsValid() && b.GetSize() >= need) return true;
    b.Destroy();
    b.Create(need, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
             gpuOnly ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE : VMA_MEMORY_USAGE_AUTO_PREFER_HOST, gpuOnly);
    return b.IsValid();
}

// An acceleration structure may be built into an object created LARGER than the
// build needs, so the object is kept until it has to grow — a per-frame create
// would be cheap, but not free, and the size only moves when the visible set does.
bool EnsureAS(VkAccelerationStructureKHR& as, CVulkanBuffer& buf, VkDeviceSize& cap,
              VkDeviceSize need, VkAccelerationStructureTypeKHR type)
{
    const RTScene::ASFns& F = RTScene::Fns();
    if (as != VK_NULL_HANDLE && cap >= need) return true;
    if (as != VK_NULL_HANDLE) { F.destroy(VulkanHW.m_Device, as, nullptr); as = VK_NULL_HANDLE; }
    const VkDeviceSize alloc = _max<VkDeviceSize>(need + need / 2 + 4096, 4096);
    if (!EnsureBuffer(buf, alloc, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, true)) return false;
    VkAccelerationStructureCreateInfoKHR ci = {};
    ci.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    ci.buffer = buf.GetHandle();
    ci.size   = alloc;
    ci.type   = type;
    if (F.create(VulkanHW.m_Device, &ci, nullptr, &as) != VK_SUCCESS) { as = VK_NULL_HANDLE; return false; }
    cap = alloc;
    return true;
}

void DestroySlot(Slot& s)
{
    const RTScene::ASFns& F = RTScene::Fns();
    if (s.blas != VK_NULL_HANDLE && F.destroy) { F.destroy(VulkanHW.m_Device, s.blas, nullptr); s.blas = VK_NULL_HANDLE; }
    if (s.tlas != VK_NULL_HANDLE && F.destroy) { F.destroy(VulkanHW.m_Device, s.tlas, nullptr); s.tlas = VK_NULL_HANDLE; }
    s.blasBuf.Destroy(); s.tlasBuf.Destroy(); s.instBuf.Destroy(); s.scratch.Destroy();
    s.blasCap = s.tlasCap = 0;
    s.tris = 0;
}

}   // anon namespace

bool Wanted()
{
    return ps_r_rt_dyn != 0 && ps_r_rtao != 0 && ps_r_preskin != 0 && RTScene::Supported();
}

bool                       Ready()      { return s_ready && s_frame == (u64)RV().frameIndex && s_cur != ~0u; }
VkAccelerationStructureKHR TLAS()       { return Ready() ? s_slot[s_cur].tlas : VK_NULL_HANDLE; }
u32                        TriCount()   { return Ready() ? s_slot[s_cur].tris : 0; }
u32                        Generation() { return s_gen; }

void Update(VkCommandBuffer cmd)
{
    // SSAOPass::Execute runs twice a frame (home pass + the Stage D clone); the
    // structure is per frame, so the second call finds it already built.
    if (s_frame == (u64)RV().frameIndex) return;
    s_frame = (u64)RV().frameIndex;
    s_ready = false;

    if (!Wanted() || !RTScene::Ready()) return;
    const RTScene::ASFns& F = RTScene::Fns();
    if (!F.create || !F.cmdBuild) return;

    const xr_vector<PreSkinRtLeaf>& leaves = Skinned_PreSkinRtLeaves();
    const VkDeviceAddress pool = Skinned_PreSkinPoolAddress();
    if (leaves.empty() || pool == 0) return;   // nothing dynamic: consumers keep the static TLAS

    const u32 n = _min((u32)leaves.size(), kMaxLeaves);
    s_geo.resize(n); s_range.resize(n); s_prim.resize(n);
    u32 tris = 0;
    for (u32 i = 0; i < n; ++i) {
        const PreSkinRtLeaf& L = leaves[i];
        VkAccelerationStructureGeometryKHR& g = s_geo[i];
        g = {};
        g.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags        = VK_GEOMETRY_OPAQUE_BIT_KHR;   // AO wants a surface, not an alpha test
        g.geometry.triangles.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        g.geometry.triangles.vertexData.deviceAddress = pool + VkDeviceSize(L.poolFirst) * 36u;   // vertHW_1W: position first
        g.geometry.triangles.vertexStride = 36;
        g.geometry.triangles.maxVertex    = _max(1u, L.vertCount) - 1;
        g.geometry.triangles.indexType    = L.iType;
        g.geometry.triangles.indexData.deviceAddress = L.ibAddr;
        s_prim[i]  = L.indexCount / 3;
        s_range[i] = { s_prim[i], 0, 0, 0 };
        tris += s_prim[i];
    }
    if (!tris) return;

    const u32 slot = CommandManager.GetCurrentFrame() % kFramesInFlight;
    Slot& s = s_slot[slot];

    // ---- BLAS sizes ------------------------------------------------------
    VkAccelerationStructureBuildGeometryInfoKHR bi = {};
    bi.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    bi.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    bi.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = n;
    bi.pGeometries   = s_geo.data();
    VkAccelerationStructureBuildSizesInfoKHR sz = {};
    sz.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    F.sizes(VulkanHW.m_Device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, s_prim.data(), &sz);

    // ---- TLAS sizes (static instances + this one) -------------------------
    VkAccelerationStructureInstanceKHR inst[3] = {};
    u32 nInst = RTScene::StaticInstances(inst);

    VkAccelerationStructureGeometryKHR tgeo = {};
    tgeo.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.flags        = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR tbi = {};
    tbi.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    tbi.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tbi.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tbi.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tbi.geometryCount = 1;
    tbi.pGeometries   = &tgeo;
    const u32 nInstTotal = nInst + 1;
    VkAccelerationStructureBuildSizesInfoKHR tsz = {};
    tsz.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    F.sizes(VulkanHW.m_Device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tbi, &nInstTotal, &tsz);

    const u32 align = _max(256u, RTScene::ScratchAlign());
    const VkDeviceSize scratchNeed = AlignUp(_max(sz.buildScratchSize, tsz.buildScratchSize), align) + align;
    const bool ok =
        EnsureAS(s.blas, s.blasBuf, s.blasCap, sz.accelerationStructureSize,  VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR) &&
        EnsureAS(s.tlas, s.tlasBuf, s.tlasCap, tsz.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR) &&
        EnsureBuffer(s.scratch, scratchNeed, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) &&
        EnsureBuffer(s.instBuf, 3 * sizeof(VkAccelerationStructureInstanceKHR) + 16,
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, false);
    if (!ok) {
        static bool s_said = false;
        if (!s_said) { s_said = true; Msg("![VK RTDyn] AS alloc failed — dynamics stay out of the BVH"); }
        return;
    }

    const int zone = VK::Prof::ZoneBegin(cmd, "RT/BuildDyn");

    // The pool was written by the PreSkin compute pass; its own barrier only
    // covered vertex-attribute reads. Build-input reads happen at the AS-build
    // stage as SHADER_READ.
    VK::MemoryBarrier(cmd,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT);

    const VkDeviceSize scratchAddr = AlignUp(s.scratch.GetDeviceAddress(), align);
    bi.dstAccelerationStructure  = s.blas;
    bi.scratchData.deviceAddress = scratchAddr;
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = s_range.data();
    F.cmdBuild(cmd, 1, &bi, &pRange);

    // TLAS reads the BLAS and reuses the scratch → READ|WRITE (same lesson as vk_rt_scene, 20-08).
    VK::MemoryBarrier(cmd,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);

    // The dynamic instance: identity (the pool is world-space), mask 0x04, and a
    // custom index no static triangle can have — rtao.frag reads it to skip the
    // back-face rule, whose winding guarantee holds for the render world only.
    {
        VkAccelerationStructureDeviceAddressInfoKHR ai = {};
        ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        ai.accelerationStructure = s.blas;
        VkAccelerationStructureInstanceKHR& d = inst[nInst];
        d = {};
        d.transform.matrix[0][0] = 1.f; d.transform.matrix[1][1] = 1.f; d.transform.matrix[2][2] = 1.f;
        d.instanceCustomIndex = 0xFFFFFFu;
        d.mask  = 0x04u;
        d.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        d.accelerationStructureReference = F.address(VulkanHW.m_Device, &ai);
        ++nInst;
    }
    // Instance data must sit on a 16-byte address (VUID 03715 — the static path's
    // buffer came out 8-aligned once); write at the aligned offset inside the buffer.
    const VkDeviceAddress instBase    = s.instBuf.GetDeviceAddress();
    const VkDeviceAddress instAligned = AlignUp(instBase, 16);
    {
        u8* dst = (u8*)s.instBuf.Map();
        if (!dst) { VK::Prof::ZoneEnd(cmd, zone); return; }
        memcpy(dst + (instAligned - instBase), inst, nInst * sizeof(VkAccelerationStructureInstanceKHR));
        s.instBuf.Flush();
        s.instBuf.Unmap();
    }
    tgeo.geometry.instances.data.deviceAddress = instAligned;
    tbi.dstAccelerationStructure  = s.tlas;
    tbi.scratchData.deviceAddress = scratchAddr;
    VkAccelerationStructureBuildRangeInfoKHR trange = { nInst, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* pTRange = &trange;
    F.cmdBuild(cmd, 1, &tbi, &pTRange);

    VK::MemoryBarrier(cmd,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    VK::Prof::ZoneEnd(cmd, zone);

    s.tris   = tris;
    s_cur    = slot;
    s_ready  = true;
    s_leaves = n;
    s_tris   = tris;
    s_blasMB = float(sz.accelerationStructureSize) / (1024.f * 1024.f);
    ++s_gen;

    static bool s_first = false;
    if (!s_first) {
        s_first = true;
        Msg("[VK RTDyn] first build: %u leaves, %u tris | blas %.2f MB (obj %.2f) tlas %u KB scratch %.2f MB | %u static instance(s)",
            n, tris, s_blasMB, float(s.blasCap) / (1024.f * 1024.f), u32(tsz.accelerationStructureSize / 1024),
            float(scratchNeed) / (1024.f * 1024.f), nInst - 1);
    }
    static u32 s_pulse = 0;
    if (++s_pulse % 601 == 1)
        Msg("[VK RTDyn] leaves=%u tris=%u blas=%.2f MB", n, tris, s_blasMB);
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    for (u32 i = 0; i < kFramesInFlight; ++i) DestroySlot(s_slot[i]);
    s_cur = ~0u; s_ready = false;
}

void OnLevelUnload()
{
    // The structures reference the pool (which survives) and a leaf list that is
    // about to die; nothing here outlives the frame, so dropping the ready flag
    // is enough — the slots are rebuilt on the first frame of the next level.
    s_ready = false;
    s_cur   = ~0u;
}

}}  // namespace VK::RTDyn
