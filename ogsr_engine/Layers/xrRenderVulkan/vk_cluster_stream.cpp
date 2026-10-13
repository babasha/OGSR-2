// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// Cluster-LOD page streaming (Stage B) — see vk_cluster_stream.h for the design.

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_cluster_stream.h"
#include "vk_buffer.h"
#include "vk_command_buffer.h"
#include "vk_profiler.h"   // VK_CPU_PROBE — per-frame CPU attribution
#include "../../xr_3da/device.h"   // RV().frameIndex / dwTimeGlobal
#include <unordered_map>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cmath>

extern int ps_r_clpage;          // 0 = eager install-all (pre-streaming behavior), 1 = stream by budget
extern int ps_r_clpage_budget;   // MB for the resident page pool (pinned pages always fit)
extern int ps_r_profiler;
extern int ps_r_clpage_map;      // pinned install uploads out of a mapping instead of fread
extern int ps_r_cl_audit;        // heavyweight streaming forensics (white-polygon hunt) — read at level load

namespace VK { namespace ClusterStream {

namespace {

using WorldGPU::GpuMeshMeta;
using WorldGPU::kErrInf;

constexpr u32 kNone             = 0xFFFFFFFFu;
constexpr u32 kMaxRequests      = 4096;    // GPU request ring entries per frame
constexpr u32 kMaxInstallsTick  = 12;      // page installs recorded per frame (1.5 MB staged)
constexpr u32 kMaxIoInflight    = 64;      // reads queued to the IO thread
constexpr u32 kSlotRetireFrames = CVulkanCommandManager::FRAMES_IN_FLIGHT + 2;  // evicted slot reuse delay
constexpr float kPinFraction    = 0.15f;   // coarse slice pinned on top of the mandatory INF roots
constexpr float kPrefetchMargin = 0.7f;    // mirrored in world_cull.comp (request before the flip)

// FNV over the bitwise (sphere, error) pair — the same identity the cut
// complementarity invariant uses: entries sharing it flip atomically.
u64 KeySE(const Fvector4& s, float e)
{
    u64 h = 1469598103934665603ull;
    auto mix = [&h](u32 v) { h = (h ^ v) * 1099511628211ull; };
    mix(*(const u32*)&s.x); mix(*(const u32*)&s.y); mix(*(const u32*)&s.z); mix(*(const u32*)&s.w);
    mix(*(const u32*)&e);
    return h;
}

u32 MortonSpread(u32 v)   // 10 bits -> 30 bits
{
    v &= 0x3FF; v = (v | (v << 16)) & 0x030000FF; v = (v | (v << 8)) & 0x0300F00F;
    v = (v | (v << 4)) & 0x030C30C3; v = (v | (v << 2)) & 0x09249249;
    return v;
}

// ============================================================================
// Runtime state
// ============================================================================
struct PageState {
    u32 flags = 0, byteOff = 0, byteSize = 0;
    u32 vbByteOff = 0, vbByteSize = 0;   // slice 2: vertex payload in the vb blob
    u32 slot = kNone;        // slot in its type's IB pool
    u32 vbSlot = kNone;      // slot in the VB pool (kNone when raw / eager)
    u32 lastTouch = 0;       // RV().frameIndex of the last GPU touch (readback)
    u32 queuedFrame = 0;     // RV().frameIndex when `queued` was set (leak watchdog)
    u64 hashIb = 0, hashVb = 0;   // payload hashes at install time (GPU slot audit)
    u8  resident = 0;
    u8  queued = 0;          // read scheduled / in flight / pending install
    xr_vector<u32> entries;  // final-meta entry ids in this page
    xr_vector<u32> groups;   // distinct member groups with entries here
};
struct GroupState {
    xr_vector<u32> pages;    // distinct pages of member entries
    xr_vector<u32> parents;  // entries whose childGroup == this group
    xr_vector<u32> members;  // entries whose memberGroup == this group
    u32 missing = 0;         // non-resident pages count
    u8  live = 0;            // all pages resident AND all parents live
    u8  inWl = 0;
};

bool s_active    = false;
bool s_streaming = false;    // false = eager install-all (no feedback machinery)
u32  s_total     = 0;        // final meta entries

xr_vector<PageState>  s_pages;
xr_vector<GroupState> s_groups;
xr_vector<u32> s_entryPage;    // final-meta entry -> page
xr_vector<u32> s_entryMember;  // -> member group (kNone = root / plain)
xr_vector<u32> s_entryChild;   // -> group it refines into (kNone = finest / plain)

// GPU buffers
CVulkanBuffer* s_pool[2]   = {};   // [0]=u16 pool, [1]=u32 pool (index buffers)
CVulkanBuffer* s_vbPool    = nullptr;   // repacked vertex pool (slice 2; eager-resident)
CVulkanBuffer* s_bits      = nullptr;   // 2 bits/entry packed 16-per-u32
CVulkanBuffer* s_slotBase  = nullptr;   // per-page PAIRS: [2p]=IB base (index units), [2p+1]=VB base (verts)
CVulkanBuffer* s_requests  = nullptr;   // [0]=count, then kMaxRequests items
CVulkanBuffer* s_touched   = nullptr;   // page bitset
CVulkanBuffer* s_stagePages = nullptr;  // host ring: FRAMES x kMaxInstallsTick pages
CVulkanBuffer* s_stageState = nullptr;  // host ring: FRAMES x (bits + slotBase)

// Feedback readback ring (CPU READS this → must be HOST_ACCESS_RANDOM, not the
// write-combined SEQUENTIAL_WRITE memory CVulkanBuffer picks; raw VMA like the
// world_gpu occlusion-stats readback).
VkBuffer      s_readback      = VK_NULL_HANDLE;
VmaAllocation s_readbackAlloc = VK_NULL_HANDLE;
u8*           s_readbackPtr   = nullptr;

xr_vector<u32> s_bitsHost;      // CPU mirror of the 2-bit state
xr_vector<u32> s_slotBaseHost;  // CPU mirror of per-page bases
bool s_stateDirty = false;      // bits/slotBase need re-upload this frame

u32 s_touchedWords = 0;
VkDeviceSize s_readbackStride = 0;   // per-frame-slot bytes in s_readback

// Slot pools (streaming mode): [0]=u16 IB, [1]=u32 IB, [2]=VB (kPageVbBytes each)
constexpr u32 kSelVB = 2;
constexpr u32 kVbSlotVerts = kPageVbBytes / kVbStride;   // 12288
struct FreeSlot { u32 slot; u32 retire; };
xr_vector<FreeSlot> s_freeSlots[3];
u32 s_slotCount[3] = { 0, 0, 0 };

// IO thread
struct IoJob   { u32 page; u32 urgency; };
struct IoDone  { u32 page; xr_vector<u8> data; };
std::thread             s_ioThread;
std::mutex              s_ioMutex;
std::condition_variable s_ioCv;
xr_vector<IoJob>        s_ioQueue;     // sorted ascending by urgency; worker pops back
xr_vector<IoDone>       s_ioDone;
bool                    s_ioRun = false;
FILE*                   s_ioFile = nullptr;
u64                     s_blobOff[2] = { 0, 0 };
u64                     s_blobOffVb = 0;
string_path             s_filePath = "";

// Installs pending GPU copy this frame (data staged in RecordFrameOps).
// data = [ibBytes of index payload][vbBytes of vertex payload] (slice 2).
struct InstallOp { u32 type; VkDeviceSize dstOff; VkDeviceSize vbDstOff; u32 ibBytes; u32 vbBytes; xr_vector<u8> data; };
xr_vector<InstallOp> s_ops;
xr_vector<IoDone>    s_doneBacklog;   // completed reads waiting for a slot / install budget

// Liveness worklist
xr_vector<u32> s_wl;

// Per-tick visited epoch for the transitive want-chase (request decode).
xr_vector<u32> s_groupSeen;
u32 s_tickEpoch = 0;

// RESIDENCY GENERATION - bumped whenever a page comes in or is evicted.
// It exists for one consumer that is NOT the renderer: the RT scene builds its
// BLAS out of whatever is resident AT THAT MOMENT and then never looks again
// while the player stands still (its stale test knows about camera drift, bubble
// radius and r_rt_lod - and nothing about streaming). The first build lands
// seconds after a load, when most of the level has not paged in, so a house the
// player is standing in can be ABSENT from the traced world and every GI ray
// flies out through its walls into open sky. One counter is all that pass needs
// to notice the world changed under it.
u32 s_resGen = 0;

// Stats
u32 s_statInstalled = 0, s_statEvicted = 0, s_statStarved = 0, s_statReqs = 0;
u32 s_statUniq = 0, s_statStuck = 0, s_statStuckG = 0xFFFFFFFFu;   // stuck = dead child group, chase found nothing
u32 s_statLogLast = 0;

// ---- GPU slot audit (white-polygon hunt 17-07) ------------------------------
// The remaining bug classes all live past the CPU mirrors: (1) bad bake payload
// only VISIBLE in streaming (an index past the page's own verts reads valid
// neighbor verts in the eager blob but garbage in a slot pool), (2) GPU pool
// bytes differing from what the install staged (copy/staging corruption),
// (3) GPU bits/slotBase diverging from the host mirror (a lost upload).
// AuditPagePayload catches (1) at install; the copy-back hash audit catches
// (2); a forced periodic state re-upload heals-and-fingers (3).
VkBuffer      s_auditBuf   = VK_NULL_HANDLE;
VmaAllocation s_auditAlloc = VK_NULL_HANDLE;
u8*           s_auditPtr   = nullptr;
struct AuditPending { u32 page = kNone; u32 ibBytes = 0, vbBytes = 0; u32 slot = kNone, vbSlot = kNone; u64 hIb = 0, hVb = 0; };
AuditPending  s_auditPend[CVulkanCommandManager::FRAMES_IN_FLIGHT];
u32           s_auditCursor = 1;                 // round-robin over pages
u32           s_auditOk = 0, s_auditBad = 0;     // cumulative slot verifications
u32           s_payloadBad = 0, s_payloadBadShown = 0;   // pages failing the install audit

u64 HashBytes(const void* p, size_t n)
{
    u64 h = 1469598103934665603ull;
    const u8* b = (const u8*)p;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { u64 v; memcpy(&v, b + i, 8); h = (h ^ v) * 1099511628211ull; }
    for (; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// ----------------------------------------------------------------------------
inline u32 PageType(const PageState& p) { return (p.flags & kPageIB32) ? 1u : 0u; }
inline u32 IdxPerSlot(u32 type) { return kPageBytes / (type ? 4u : 2u); }

// Deterministic payload audit at install time. A repacked page is self-
// contained: every index must address a vert INSIDE its own vertex payload
// (page-local contract, bake v12) and positions must be sane floats. An index
// past vbByteSize is exactly the "white stretched polygon": in the eager blob
// it lands on valid neighbor verts (invisible), in a streaming slot it reads
// whatever the neighboring slot bytes happen to be.
void AuditPagePayload(u32 page, u32 type, const u8* ib, u32 ibBytes, const u8* vb, u32 vbBytes)
{
    if (ibBytes > kPageBytes || vbBytes > kPageVbBytes) {
        ++s_payloadBad;
        Msg("![VK ClPage-AUDIT] page %u payload EXCEEDS SLOT: ib %u/%u vb %u/%u — install corrupts the NEIGHBOR slot!",
            page, ibBytes, kPageBytes, vbBytes, kPageVbBytes);
        return;
    }
    if (!vbBytes) return;   // raw page: absolute indices into the original VB — no page-local contract
    const u32 vertLimit = vbBytes / kVbStride;
    u32 badIdx = 0, maxIdx = 0, badVert = 0;
    if (type) {
        const u32* idx = (const u32*)ib;
        for (u32 k = 0; k < ibBytes / 4; ++k) { const u32 v = idx[k]; if (v > maxIdx) maxIdx = v; if (v >= vertLimit) ++badIdx; }
    } else {
        const u16* idx = (const u16*)ib;
        for (u32 k = 0; k < ibBytes / 2; ++k) { const u32 v = idx[k]; if (v > maxIdx) maxIdx = v; if (v >= vertLimit) ++badIdx; }
    }
    for (u32 v = 0; v < vertLimit; ++v) {
        const float* pos = (const float*)(vb + (size_t)v * kVbStride);
        if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])
            || fabsf(pos[0]) > 16384.f || fabsf(pos[1]) > 16384.f || fabsf(pos[2]) > 16384.f)
            ++badVert;
    }
    if (badIdx || badVert) {
        ++s_payloadBad;
        if (s_payloadBadShown++ < 16)
            Msg("![VK ClPage-AUDIT] page %u BAD PAYLOAD: %u oob indices (max %u, limit %u), %u insane verts (ib %u vb %u bytes)",
                page, badIdx, maxIdx, vertLimit, badVert, ibBytes, vbBytes);
    }
}

inline bool EntryLive(u32 e)
{
    if (!s_pages[s_entryPage[e]].resident) return false;
    const u32 g = s_entryMember[e];
    return g == kNone || s_groups[g].live != 0;
}

inline void UpdateEntryBits(u32 e)
{
    const u32 child = s_entryChild[e];
    u32 v = EntryLive(e) ? 1u : 0u;
    if (child != kNone && !s_groups[child].live) v |= 2u;
    const u32 word = e >> 4u, shift = (e & 15u) * 2u;
    const u32 old = s_bitsHost[word];
    const u32 nw  = (old & ~(3u << shift)) | (v << shift);
    if (nw != old) { s_bitsHost[word] = nw; s_stateDirty = true; }
}

inline void WlPush(u32 g)
{
    if (g == kNone || s_groups[g].inWl) return;
    s_groups[g].inWl = 1; s_wl.push_back(g);
}

u32 s_liveFlips = 0;   // liveness changes in the last propagation (resync diag)

// Fixpoint propagation of group liveness. A group's liveness feeds its members'
// drawable bits (they may only draw when their whole cohort can) and its
// parents' leaf bits (parents draw AS the cut leaf while the cohort is broken).
// Changes cascade toward finer levels via the members (each member is a parent
// of the group it refines into). DAG depth <= 12 bounds the passes.
void PropagateLiveness()
{
    size_t head = 0;
    while (head < s_wl.size()) {
        const u32 g = s_wl[head++];
        GroupState& G = s_groups[g];
        G.inWl = 0;
        bool live = (G.missing == 0);
        if (live)
            for (u32 p : G.parents)
                if (!EntryLive(p)) { live = false; break; }
        if ((u8)live == G.live) continue;
        G.live = (u8)live;
        ++s_liveFlips;
        for (u32 p : G.parents) UpdateEntryBits(p);          // leaf flips
        for (u32 m : G.members) {
            UpdateEntryBits(m);                              // drawable flips
            WlPush(s_entryChild[m]);                         // cascade finer
        }
    }
    s_wl.clear();
}

// Full state resync — the SAFETY NET for the 16-07 stuck-web symptom (groups
// dead with everything resident): recount every group's missing pages from
// actual residency and re-run the liveness fixpoint over ALL groups. Any
// correction is a state-machine bug being healed — the log line quantifies it
// (miss-counter drift vs pure propagation misses) so the root cause stays
// visible while the game keeps converging. ~few ms, called from the 3s tick.
void FullResync(u32& fixedMiss, u32& liveFlips)
{
    fixedMiss = 0;
    for (u32 g = 0; g < (u32)s_groups.size(); ++g) {
        GroupState& G = s_groups[g];
        u32 rm = 0;
        for (u32 p : G.pages) if (!s_pages[p].resident) ++rm;
        if (rm != G.missing) { G.missing = rm; ++fixedMiss; }
        WlPush(g);
    }
    s_liveFlips = 0;
    PropagateLiveness();
    liveFlips = s_liveFlips;
}

void FlipPage(u32 p, bool nowResident)
{
    PageState& ps = s_pages[p];
    if (ps.resident == (u8)nowResident) return;
    ps.resident = (u8)nowResident;
    ++s_resGen;
    for (u32 g : ps.groups) {
        s_groups[g].missing += nowResident ? -1 : 1;
        WlPush(g);
    }
    for (u32 e : ps.entries) {
        UpdateEntryBits(e);
        WlPush(s_entryChild[e]);   // these entries are parents of their child groups
    }
}

// ----------------------------------------------------------------------------
void IoThreadMain()
{
    set_current_thread_name("VK ClusterIO");   // см. cpu_budget: безымянный поток нечитаем в отчёте
    for (;;) {
        IoJob job;
        {
            std::unique_lock<std::mutex> lk(s_ioMutex);
            s_ioCv.wait(lk, [] { return !s_ioRun || !s_ioQueue.empty(); });
            if (!s_ioRun) return;
            job = s_ioQueue.back(); s_ioQueue.pop_back();
        }
        const PageState& ps = s_pages[job.page];   // geometry fields are immutable
        IoDone done; done.page = job.page;
        const u32 vbBytes = s_vbPool ? ps.vbByteSize : 0;   // slice 2: page atomic = IB + VB chunks travel together
        done.data.resize((size_t)ps.byteSize + vbBytes);
        bool ok = false;
        if (s_ioFile && ps.byteSize) {
            if (_fseeki64(s_ioFile, (long long)(s_blobOff[PageType(ps)] + ps.byteOff), SEEK_SET) == 0)
                ok = fread(done.data.data(), 1, ps.byteSize, s_ioFile) == ps.byteSize;
            if (ok && vbBytes)
                ok = _fseeki64(s_ioFile, (long long)(s_blobOffVb + ps.vbByteOff), SEEK_SET) == 0
                  && fread(done.data.data() + ps.byteSize, 1, vbBytes, s_ioFile) == vbBytes;
        }
        if (!ok) done.data.clear();   // consumer un-queues and logs
        {
            std::lock_guard<std::mutex> lk(s_ioMutex);
            s_ioDone.push_back(std::move(done));
        }
    }
}

u32 AllocSlot(u32 type)
{
    auto& fl = s_freeSlots[type];
    for (size_t i = 0; i < fl.size(); ++i) {
        if (RV().frameIndex >= fl[i].retire + kSlotRetireFrames) {
            const u32 s = fl[i].slot;
            fl[i] = fl.back(); fl.pop_back();
            return s;
        }
    }
    return kNone;
}

// Evict the least-recently-touched resident non-pinned page matching `sel`
// (0/1 = an IB type — a page whose u16/u32 slot we need; kSelVB = any page
// holding a VB slot). Frees BOTH of the page's slots (a page is atomic). Pages
// touched within the last few frames are protected (they're in the live cut —
// evicting them would just bounce). Returns false when nothing is evictable
// (oversubscription: requests wait, coarser leaves keep covering).
bool EvictOne(u32 sel)
{
    u32 best = kNone, bestTouch = 0xFFFFFFFFu;
    for (u32 p = 1; p < (u32)s_pages.size(); ++p) {
        PageState& ps = s_pages[p];
        if (!ps.resident || (ps.flags & kPagePinned)) continue;
        if (sel == kSelVB ? ps.vbSlot == kNone : PageType(ps) != sel) continue;
        if (ps.lastTouch + 8 > RV().frameIndex) continue;   // in the live cut
        if (ps.lastTouch < bestTouch) { bestTouch = ps.lastTouch; best = p; }
    }
    if (best == kNone) { ++s_statStarved; return false; }
    PageState& ps = s_pages[best];
    FlipPage(best, false);
    s_freeSlots[PageType(ps)].push_back({ ps.slot, RV().frameIndex });
    ps.slot = kNone;
    if (ps.vbSlot != kNone) {
        s_freeSlots[kSelVB].push_back({ ps.vbSlot, RV().frameIndex });
        ps.vbSlot = kNone;
    }
    ++s_statEvicted;
    return true;
}

void DestroyBuffers()
{
    auto del = [](CVulkanBuffer*& b) { if (b) { if (b->IsMapped()) b->Unmap(); xr_delete(b); } };
    del(s_pool[0]); del(s_pool[1]); del(s_vbPool);
    del(s_bits); del(s_slotBase); del(s_requests); del(s_touched);
    del(s_stagePages); del(s_stageState);
    if (s_readback) {
        VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, s_readback, s_readbackAlloc);
        s_readback = VK_NULL_HANDLE; s_readbackAlloc = VK_NULL_HANDLE; s_readbackPtr = nullptr;
    }
    if (s_auditBuf) {
        VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, s_auditBuf, s_auditAlloc);
        s_auditBuf = VK_NULL_HANDLE; s_auditAlloc = VK_NULL_HANDLE; s_auditPtr = nullptr;
    }
    for (auto& ap : s_auditPend) ap = AuditPending{};
    s_auditCursor = 1;
    s_auditOk = s_auditBad = s_payloadBad = s_payloadBadShown = 0;
}

} // anonymous namespace

// ============================================================================
// Bake-time page assembly
// ============================================================================
void AssemblePages(xr_vector<GpuMeshMeta>& meta, const xr_vector<u8>& entryIb32,
                   const xr_vector<u32>& entryMesh,
                   const xr_vector<MeshVbInfo>& meshInfo,
                   const xr_vector<VbSource>& sources,
                   xr_vector<u16>& idx16, xr_vector<u32>& idx32,
                   xr_vector<u8>& outVbBlob,
                   xr_vector<PageRec>& outPages)
{
    outPages.clear();
    outVbBlob.clear();
    outPages.push_back({ kPagePinned, 0, 0, 0, 0 });   // page 0 = identity (plain meshes)
    const u32 n = (u32)meta.size();
    if (n == 0) return;

    // ---- Slice 2: per-entry vertex-repack eligibility -------------------------
    // The flag must stay uniform per MESH: a mesh's draw group binds ONE vertex
    // buffer (PoolVB for repacked, the original pool VB for raw) for all of its
    // entries, and the cull adds the page VB base unconditionally — so a mesh
    // may never mix repacked and raw entries. The out-of-source fetch scan is
    // paranoia (the bake just built these indices from validated data), but a
    // silent OOB read here would corrupt the blob instead of crashing.
    xr_vector<u8> meshOk(meshInfo.size(), 0);
    for (u32 mi = 0; mi < (u32)meshInfo.size(); ++mi)
        meshOk[mi] = meshInfo[mi].repack && meshInfo[mi].srcId < (u32)sources.size()
                  && sources[meshInfo[mi].srcId].stride == kVbStride;
    u32 oobMeshes = 0;
    for (u32 i = 0; i < n; ++i) {
        const u32 mid = i < (u32)entryMesh.size() ? entryMesh[i] : (u32)meshOk.size();
        if (mid >= (u32)meshOk.size() || !meshOk[mid]) continue;
        const VbSource& src = sources[meshInfo[mid].srcId];
        u32 maxIdx = 0;
        if (entryIb32[i])
            for (u32 k = 0; k < meta[i].index_count; ++k) maxIdx = _max(maxIdx, idx32[meta[i].ib_first + k]);
        else
            for (u32 k = 0; k < meta[i].index_count; ++k) maxIdx = _max(maxIdx, (u32)idx16[meta[i].ib_first + k]);
        if ((u64)(meta[i].first_vertex + maxIdx + 1) * src.stride > src.bytes) { meshOk[mid] = 0; ++oobMeshes; }
    }
    xr_vector<u8> entryRepack(n, 0);
    for (u32 i = 0; i < n; ++i) {
        const u32 mid = i < (u32)entryMesh.size() ? entryMesh[i] : (u32)meshOk.size();
        entryRepack[i] = mid < (u32)meshOk.size() ? meshOk[mid] : 0;
    }
    if (oobMeshes)
        Msg("![VK ClPage] %u meshes with out-of-source vertex fetches — left on their original VB", oobMeshes);

    // Member groups: entries sharing the bitwise (lodParent, parentError).
    std::unordered_map<u64, u32> gid;
    gid.reserve(n / 4);
    xr_vector<u32> memberOf(n, kNone);
    xr_vector<xr_vector<u32>> members;
    for (u32 i = 0; i < n; ++i) {
        if (meta[i].parentError >= kErrInf) continue;   // root — packed standalone
        const u64 k = KeySE(meta[i].lodParent, meta[i].parentError);
        auto ins = gid.emplace(k, (u32)members.size());
        if (ins.second) members.emplace_back();
        memberOf[i] = ins.first->second;
        members[ins.first->second].push_back(i);
    }

    // Morton order over the entry sphere centres (locality for streamed pages).
    Fvector lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
    for (u32 i = 0; i < n; ++i) {
        lo.x = _min(lo.x, meta[i].sphere_P.x); hi.x = _max(hi.x, meta[i].sphere_P.x);
        lo.y = _min(lo.y, meta[i].sphere_P.y); hi.y = _max(hi.y, meta[i].sphere_P.y);
        lo.z = _min(lo.z, meta[i].sphere_P.z); hi.z = _max(hi.z, meta[i].sphere_P.z);
    }
    Fvector ext{ _max(hi.x - lo.x, 1.f), _max(hi.y - lo.y, 1.f), _max(hi.z - lo.z, 1.f) };
    auto morton = [&](const Fvector& p) {
        const u32 x = (u32)_min(1023.f, _max(0.f, (p.x - lo.x) / ext.x * 1023.f));
        const u32 y = (u32)_min(1023.f, _max(0.f, (p.y - lo.y) / ext.y * 1023.f));
        const u32 z = (u32)_min(1023.f, _max(0.f, (p.z - lo.z) / ext.z * 1023.f));
        return (MortonSpread(x) << 2) | (MortonSpread(y) << 1) | MortonSpread(z);
    };

    // Pack items: a member group (entries stay adjacent — the cohort flips
    // atomically, so its pages want to arrive together) or a root singleton.
    // Slice 2: cohorts split by the repack flag (a KeySE collision can merge
    // groups of a repackable and a raw mesh) — pages must stay homogeneous.
    struct Item { u32 morton; float coarse; u32 type; u64 bytes; bool pinned; bool repack; xr_vector<u32> entries; };
    xr_vector<Item> items;
    items.reserve(members.size() + 256);
    u64 totalBytes = 0, rootBytes = 0;
    auto entryBytes = [&](u32 i) { return (u64)meta[i].index_count * (entryIb32[i] ? 4u : 2u); };

    for (u32 g = 0; g < (u32)members.size(); ++g) {
        for (int rp = 0; rp < 2; ++rp) {
            Item it{}; it.repack = rp != 0;
            for (u32 e : members[g]) if ((entryRepack[e] != 0) == it.repack) it.entries.push_back(e);
            if (it.entries.empty()) continue;
            Fvector c{ 0, 0, 0 }; float coarse = 0.f;
            for (u32 e : it.entries) {
                it.bytes += entryBytes(e);
                coarse = _max(coarse, meta[e].selfError);
                c.add(meta[e].sphere_P);
            }
            c.div((float)it.entries.size());
            it.morton = morton(c); it.coarse = coarse;
            it.type = entryIb32[it.entries[0]] ? 1u : 0u;
            totalBytes += it.bytes;
            items.push_back(std::move(it));
        }
    }
    for (u32 i = 0; i < n; ++i) {
        if (memberOf[i] != kNone) continue;
        Item it{}; it.entries.push_back(i);
        it.bytes = entryBytes(i); it.morton = morton(meta[i].sphere_P);
        it.coarse = kErrInf; it.pinned = true;   // roots: the ultimate fallback, mandatory
        it.type = entryIb32[i] ? 1u : 0u;
        it.repack = entryRepack[i] != 0;
        totalBytes += it.bytes; rootBytes += it.bytes;
        items.push_back(std::move(it));
    }

    // Pin the coarsest groups until the quota (on top of the mandatory roots).
    {
        u64 quota = (u64)((double)totalBytes * kPinFraction);
        quota = quota > rootBytes ? quota - rootBytes : 0;
        xr_vector<u32> order(items.size());
        for (u32 i = 0; i < (u32)order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return items[a].coarse > items[b].coarse; });
        for (u32 i : order) {
            if (quota == 0) break;
            if (items[i].pinned) continue;
            items[i].pinned = true;
            quota = quota > items[i].bytes ? quota - items[i].bytes : 0;
        }
    }

    // Order: pinned first (zone boundary is page-aligned), then morton locality.
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.type   != b.type)   return a.type < b.type;     // type-homogeneous pages
        if (a.repack != b.repack) return a.repack < b.repack; // repacked/raw never share a page
        if (a.pinned != b.pinned) return a.pinned;            // pinned zone first per type
        if (a.morton != b.morton) return a.morton < b.morton;
        return a.coarse > b.coarse;
    });

    // Emit: copy each entry's index span into the page-ordered blobs; for
    // repacked entries also copy their unique vertices into the page's vertex
    // block (PAGE-wide dedup — cohorts share boundary verts, and nearby LOD
    // levels of one mesh land in the same page via morton) and rewrite their
    // indices PAGE-LOCAL with first_vertex = 0. Max verts per page is
    // kPageVbBytes/kVbStride = 12288 < 65536, so u16 indices can't overflow.
    xr_vector<u16> new16; new16.reserve(idx16.size());
    xr_vector<u32> new32; new32.reserve(idx32.size());
    u32 curPage = kNone, curType = 0; bool curPinned = false, curRepack = false;
    u32 pageFill = 0;      // IB bytes
    u32 pageVbOff = 0;     // page's vertex-block start in outVbBlob
    u32 pageVerts = 0;     // verts emitted into the current page
    u64 pinnedBytes = 0; u32 pinnedPages = 0;
    std::unordered_map<u64, u32> pageMap;   // (srcId<<32|globalVert) -> page-local vert
    std::unordered_map<u64, u8>  entrySeen; // per-entry uniq scratch (probe pass)
    pageMap.reserve(16384); entrySeen.reserve(512);

    auto openPage = [&](u32 type, bool pinned, bool repack) {
        curPage = (u32)outPages.size(); curType = type; curPinned = pinned; curRepack = repack;
        pageFill = 0; pageVerts = 0;
        pageVbOff = (u32)outVbBlob.size();   // always kVbStride-aligned (whole verts only)
        pageMap.clear();
        PageRec r{};
        r.flags = (type ? kPageIB32 : 0u) | (pinned ? kPagePinned : 0u);
        r.byteOff = type ? (u32)(new32.size() * 4) : (u32)(new16.size() * 2);
        r.vbByteOff = pageVbOff;
        outPages.push_back(r);
        if (pinned) ++pinnedPages;
    };
    auto closePage = [&]() {
        if (curPage == kNone) return;
        outPages[curPage].byteSize = pageFill;
        outPages[curPage].vbByteSize = pageVerts * kVbStride;
        if (pageVerts) outPages[curPage].flags |= kPageHasVB;
        if (curPinned) pinnedBytes += pageFill + pageVerts * kVbStride;
        curPage = kNone;
    };

    for (const Item& it : items) {
        if (curPage == kNone || curType != it.type || curPinned != it.pinned || curRepack != it.repack)
            { closePage(); openPage(it.type, it.pinned, it.repack); }
        for (u32 e : it.entries) {
            GpuMeshMeta& m = meta[e];
            const u32 bytes = (u32)entryBytes(e);
            const VbSource* src = nullptr;
            u64 srcKeyHi = 0;
            u32 uniqEntry = 0, newVerts = 0;
            if (it.repack) {
                const u32 srcId = meshInfo[entryMesh[e]].srcId;
                src = &sources[srcId];
                srcKeyHi = (u64)srcId << 32;
                // Probe pass: unique verts of this entry, and how many are new
                // to the CURRENT page (the close decision below); after a page
                // break every unique vert is new.
                entrySeen.clear();
                for (u32 k = 0; k < m.index_count; ++k) {
                    const u32 raw = it.type ? idx32[m.ib_first + k] : (u32)idx16[m.ib_first + k];
                    const u64 key = srcKeyHi | (m.first_vertex + raw);
                    if (!entrySeen.emplace(key, u8(1)).second) continue;
                    ++uniqEntry;
                    if (pageMap.find(key) == pageMap.end()) ++newVerts;
                }
            }
            if (pageFill > 0
                && (pageFill + bytes > kPageBytes
                    || (it.repack && (pageVerts + newVerts) * kVbStride > kPageVbBytes)))
                { closePage(); openPage(it.type, it.pinned, it.repack); newVerts = uniqEntry; }
            m._pad1 = curPage;
            if (it.repack) {
                const u32 localIb = it.type ? pageFill / 4 : pageFill / 2;
                for (u32 k = 0; k < m.index_count; ++k) {
                    const u32 raw = it.type ? idx32[m.ib_first + k] : (u32)idx16[m.ib_first + k];
                    const u32 v   = m.first_vertex + raw;
                    auto ins = pageMap.emplace(srcKeyHi | v, pageVerts);
                    u32 local;
                    if (ins.second) {
                        local = pageVerts++;
                        const u8* vp = src->data + (size_t)v * kVbStride;
                        outVbBlob.insert(outVbBlob.end(), vp, vp + kVbStride);
                    } else local = ins.first->second;
                    if (it.type) new32.push_back(local); else new16.push_back((u16)local);
                }
                m.ib_first = localIb;
                m.first_vertex = 0;   // page-local; the cull adds the page VB base
            } else if (it.type) {
                const u32 local = pageFill / 4;
                new32.insert(new32.end(), idx32.begin() + m.ib_first, idx32.begin() + m.ib_first + m.index_count);
                m.ib_first = local;
            } else {
                const u32 local = pageFill / 2;
                new16.insert(new16.end(), idx16.begin() + m.ib_first, idx16.begin() + m.ib_first + m.index_count);
                m.ib_first = local;
            }
            pageFill += bytes;
        }
    }
    closePage();

    idx16.swap(new16);
    idx32.swap(new32);

    u32 vbPages = 0, rawEntries = 0;
    for (const PageRec& r : outPages) if (r.flags & kPageHasVB) ++vbPages;
    for (u32 i = 0; i < n; ++i) if (!entryRepack[i]) ++rawEntries;
    Msg("[VK ClPage] assembled %u pages (%.1f MB IB + %.1f MB VB in %u pages), pinned %u (%.1f MB: roots %.1f IB + coarse quota) | %u groups, %u entries (%u raw)",
        (u32)outPages.size() - 1, double(totalBytes) / (1024.0 * 1024.0),
        double(outVbBlob.size()) / (1024.0 * 1024.0), vbPages,
        pinnedPages, double(pinnedBytes) / (1024.0 * 1024.0), double(rootBytes) / (1024.0 * 1024.0),
        (u32)members.size(), n, rawEntries);
    VERIFY(outVbBlob.size() < (u64)0xFFFFFFFFu);   // u32 vbByteOff (real levels: <2 GB)
}

// ============================================================================
// Runtime — phase 1: pools (before draw grouping)
// ============================================================================
bool CreatePools(const xr_vector<PageRec>& pages,
                 const char* filePath, u64 blobOff16, u64 blobOff32, u64 blobOffVb,
                 const xr_vector<u16>* ramIdx16, const xr_vector<u32>* ramIdx32,
                 const xr_vector<u8>* ramVb)
{
    VK::Vram::Scope _vram_scope("Clusters");
    CTimer _cp; _cp.Start();
    float msAlloc = 0, msRead = 0, msUpload = 0;
    u32   nPinnedInstalled = 0; u64 pinnedReadBytes = 0;
    Destroy();
    const u32 nPages = _max(1u, (u32)pages.size());
    s_pages.resize(nPages);
    for (u32 p = 0; p < (u32)pages.size(); ++p) {
        s_pages[p].flags      = pages[p].flags;
        s_pages[p].byteOff    = pages[p].byteOff;
        s_pages[p].byteSize   = pages[p].byteSize;
        s_pages[p].vbByteOff  = pages[p].vbByteOff;
        s_pages[p].vbByteSize = pages[p].vbByteSize;
    }
    if (nPages <= 1) {   // no clusters on this level (or r_cluster 0): identity page only
        s_streaming = false;
        s_slotBaseHost.assign(2, 0u);
        s_pages[0].resident = 1;
        return true;
    }

    u64 blobBytes[2] = { 0, 0 }, vbBlobBytes = 0;
    u32 pageCount[2] = { 0, 0 }, pinnedCount[2] = { 0, 0 };
    u64 pinnedBytes = 0;
    for (u32 p = 1; p < nPages; ++p) {
        const u32 t = PageType(s_pages[p]);
        blobBytes[t] = _max(blobBytes[t], (u64)s_pages[p].byteOff + s_pages[p].byteSize);
        vbBlobBytes  = _max(vbBlobBytes, (u64)pages[p].vbByteOff + pages[p].vbByteSize);
        ++pageCount[t];
        if (s_pages[p].flags & kPagePinned) { ++pinnedCount[t]; pinnedBytes += s_pages[p].byteSize; }
    }

    xr_strcpy(s_filePath, filePath ? filePath : "");
    s_blobOff[0] = blobOff16; s_blobOff[1] = blobOff32; s_blobOffVb = blobOffVb;
    const bool haveFile = s_filePath[0] && (s_ioFile = fopen(XR_OSPATH(s_filePath), "rb")) != nullptr;
    const bool haveRam  = (ramIdx16 && (!blobBytes[0] || ramIdx16->size() * 2 >= blobBytes[0]))
                       && (ramIdx32 && (!blobBytes[1] || ramIdx32->size() * 4 >= blobBytes[1]))
                       && (!vbBlobBytes || (ramVb && ramVb->size() >= vbBlobBytes));
    s_streaming = ps_r_clpage != 0 && haveFile;
    if (ps_r_clpage != 0 && !haveFile)
        Msg("![VK ClPage] r_clpage=1 but the cluster cache file is unavailable — eager install-all");
    if (!haveFile && !haveRam) { Msg("![VK ClPage] no page data source — cluster path disabled"); Destroy(); return false; }

    // TRANSFER_SRC: the GPU slot audit copies resident slots back for hashing.
    const VkBufferUsageFlags poolUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                       | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;   // RT world gather reads the pool by address

    if (!s_streaming) {
        // ---- Eager: exact-size pools, everything resident (pre-streaming parity) ----
        for (u32 t = 0; t < 2; ++t) {
            if (!blobBytes[t]) continue;
            s_pool[t] = xr_new<CVulkanBuffer>();
            s_pool[t]->Create(blobBytes[t], poolUsage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
            if (!s_pool[t]->IsValid()) { Msg("![VK ClPage] pool%u alloc failed (%llu MB)", t, (unsigned long long)(blobBytes[t] >> 20)); Destroy(); return false; }
        }
        // Upload blobs: from RAM (fresh bake) or the cache file (cache hit).
        for (u32 t = 0; t < 2; ++t) {
            if (!blobBytes[t]) continue;
            if (haveRam) {
                const void* src = t ? (const void*)ramIdx32->data() : (const void*)ramIdx16->data();
                s_pool[t]->Upload(src, blobBytes[t]);
            } else {
                xr_vector<u8> tmp(blobBytes[t]);
                bool ok = _fseeki64(s_ioFile, (long long)s_blobOff[t], SEEK_SET) == 0
                       && fread(tmp.data(), 1, tmp.size(), s_ioFile) == tmp.size();
                if (!ok) { Msg("![VK ClPage] blob%u read failed — cluster path disabled", t); Destroy(); return false; }
                s_pool[t]->Upload(tmp.data(), tmp.size());
            }
        }
        s_slotBaseHost.assign((size_t)2 * nPages, 0u);
        for (u32 p = 1; p < nPages; ++p) {
            s_pages[p].resident = 1;
            s_slotBaseHost[2 * p] = s_pages[p].byteOff / (PageType(s_pages[p]) ? 4u : 2u);
        }
        s_pages[0].resident = 1;
    } else {
        // ---- Streaming: budgeted slot pools; pinned pages get permanent slots ----
        u64 budget = (u64)_max(32, ps_r_clpage_budget) << 20;
        const u64 pinnedSlots = (u64)(pinnedCount[0] + pinnedCount[1] + 1) * kPageBytes;
        if (budget < pinnedSlots + 32ull * kPageBytes) budget = pinnedSlots + 32ull * kPageBytes;
        const u64 streamBudget = budget - pinnedSlots;
        const u64 streamBytes[2] = { blobBytes[0], blobBytes[1] };   // upper bound of demand
        const u64 wantSlots = streamBudget / kPageBytes;
        // Split streamed slots by each type's share of streamable pages.
        const u32 sPages[2] = { pageCount[0] - pinnedCount[0], pageCount[1] - pinnedCount[1] };
        const u32 sTotal = _max(1u, sPages[0] + sPages[1]);
        (void)streamBytes;
        // The pools are single allocations (page-slot addressing needs one
        // buffer) — on a fragmented / near-full heap the first try can fail
        // outright (Pripyat 17-07: 479 MB VB slot pool, OUT_OF_DEVICE_MEMORY
        // on the 3rd level load of the session). Losing the WHOLE cluster path
        // to that is far worse than a tighter pool: halve the STREAM slot
        // budget and retry down to pinned-only — fewer resident slots just
        // means more eviction and coarser distant clusters.
        // Slice 2 note: the VB pool pairs one VB slot per IB slot (a resident
        // page holds one of each; pages without a VB payload leave theirs spare).
        for (u64 tryWant = wantSlots;;) {
            bool ok = true;
            for (u32 t = 0; t < 2 && ok; ++t) {
                u32 slots = pinnedCount[t] + (u32)_min<u64>(sPages[t], tryWant * sPages[t] / sTotal);
                slots = _min(slots, pageCount[t]);   // never more slots than pages
                s_slotCount[t] = slots;
                if (!slots) continue;
                s_pool[t] = xr_new<CVulkanBuffer>();
                s_pool[t]->Create((VkDeviceSize)slots * kPageBytes, poolUsage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
                if (!s_pool[t]->IsValid()) ok = false;
            }
            if (ok && vbBlobBytes) {
                s_slotCount[kSelVB] = s_slotCount[0] + s_slotCount[1];
                s_vbPool = xr_new<CVulkanBuffer>();
                s_vbPool->Create((VkDeviceSize)s_slotCount[kSelVB] * kPageVbBytes,
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                 | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
                if (!s_vbPool->IsValid()) ok = false;
            }
            if (ok) break;
            for (u32 t = 0; t < 2; ++t) if (s_pool[t]) xr_delete(s_pool[t]);
            if (s_vbPool) xr_delete(s_vbPool);
            s_slotCount[0] = s_slotCount[1] = s_slotCount[kSelVB] = 0;
            if (tryWant == 0) {
                Msg("![VK ClPage] slot pool alloc failed even at pinned-only — cluster path disabled");
                Destroy(); return false;
            }
            tryWant = (tryWant >= 128) ? tryWant / 2 : 0;   // halve; below 128 go straight to pinned-only
            Msg("![VK ClPage] slot pool alloc failed (VRAM full/fragmented) — retrying with %llu stream slots",
                (unsigned long long)tryWant);
        }
        // Install pinned pages now (level load — Upload staging is fine).
        // Sub-timers: CreatePools measured 835 ms of the 18-08 load, and this loop
        // does ~2 payload reads + 2 Uploads per PINNED page (768 of them on
        // pripyat_full) — split read from upload before touching either.
        msAlloc = _cp.GetElapsed_ms_total();
        _cp.Start();
        s_slotBaseHost.assign((size_t)2 * nPages, 0u);
        u32 next[3] = { 0, 0, 0 };

        // The install used to walk pinned pages one by one, reading each page's IB
        // payload and then its VB payload — that is THREE regions of the cache file
        // (IB16 blob, IB32 blob, VB blob) interleaved page by page, ~1500 seeks for
        // 768 pages. Measured 18-08: 664 ms for 336 MB, about 500 MB/s on an NVMe
        // that reads sequentially at 2+ GB/s. So: plan first (no IO), then run the
        // reads in FILE order, so each blob is scanned once, forward, and the OS
        // readahead applies. Slots are still handed out in page order — the
        // residency layout is exactly what it was.
        struct PinRead { u64 off; u32 page, sel, size, slot; };   // sel 0/1 = IB blob, kSelVB = VB blob
        xr_vector<PinRead> plan;
        plan.reserve((size_t)(pinnedCount[0] + pinnedCount[1]) * 2);
        for (u32 p = 1; p < nPages; ++p) {
            PageState& ps = s_pages[p];
            if (!(ps.flags & kPagePinned) || !ps.byteSize) { if (!ps.byteSize && (ps.flags & kPagePinned)) ps.resident = 1; continue; }
            const u32 t = PageType(ps);
            ps.slot = next[t]++;
            plan.push_back({ s_blobOff[t] + ps.byteOff, p, t, ps.byteSize, ps.slot });
            const u32 vbB = (s_vbPool && ps.vbByteSize) ? ps.vbByteSize : 0;
            if (vbB) {
                ps.vbSlot = next[kSelVB]++;
                plan.push_back({ s_blobOffVb + ps.vbByteOff, p, kSelVB, vbB, ps.vbSlot });
                s_slotBaseHost[2 * p + 1] = ps.vbSlot * kVbSlotVerts;
            }
            ps.resident = 1;
            s_slotBaseHost[2 * p] = ps.slot * IdxPerSlot(t);
            ++nPinnedInstalled;
        }
        std::sort(plan.begin(), plan.end(),
                  [](const PinRead& a, const PinRead& b) { return a.off < b.off; });

        // 336 MB out of a 2.2 GB file, and it used to arrive as fseek+fread into a
        // scratch buffer -- a serial read on the loading thread, followed by a SECOND
        // copy into the staging ring. Map the file instead and hand Upload the mapped
        // bytes directly: one copy, and the page faults the read really consists of
        // move onto workers -- which is where the geometry stage already learned to
        // put them (a resident page still faults into a new view).
        const u8* mapBase = nullptr;
        HANDLE    mapFile = INVALID_HANDLE_VALUE;
        HANDLE    mapObj  = nullptr;
        if (!haveRam && ps_r_clpage_map && s_filePath[0]) {
            mapFile = CreateFile(s_filePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
            if (mapFile != INVALID_HANDLE_VALUE) {
                mapObj = CreateFileMapping(mapFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
                if (mapObj) mapBase = (const u8*)MapViewOfFile(mapObj, FILE_MAP_READ, 0, 0, 0);
            }
            if (!mapBase) {
                if (mapObj) CloseHandle(mapObj);
                if (mapFile != INVALID_HANDLE_VALUE) CloseHandle(mapFile);
                mapObj = nullptr;
                mapFile = INVALID_HANDLE_VALUE;
                Msg("![VK ClPage] page cache mapping failed - falling back to fread");
            }
        }
        if (mapBase) {
            CTimer _w;
            _w.Start();
            std::atomic<size_t> cursor{0};
            std::atomic<u32>    sink{0};
            xr_vector<std::thread> warm;
            warm.reserve(16);
            for (u32 w = 0; w < 16; ++w)
                warm.emplace_back([&plan, mapBase, &cursor, &sink]() {
                    u32 acc = 0;
                    for (size_t i = cursor.fetch_add(1); i < plan.size(); i = cursor.fetch_add(1)) {
                        const PinRead& pr = plan[i];
                        const u8* p = mapBase + pr.off;
                        for (u32 o = 0; o < pr.size; o += 4096) acc += p[o];
                        if (pr.size) acc += p[pr.size - 1];
                    }
                    sink.fetch_add(acc, std::memory_order_relaxed);   // keep the loop
                });
            for (auto& t : warm) t.join();
            msRead = _w.GetElapsed_ms_total();   // the faults ARE the read
        }

        xr_vector<u8> tmp;
        for (const PinRead& r : plan) {
            const u8* src = nullptr;
            bool ok = false;
            CTimer _s; _s.Start();
            if (haveRam) {
                const u8* ram = (r.sel == kSelVB) ? (const u8*)ramVb->data()
                              : (r.sel ? (const u8*)ramIdx32->data() : (const u8*)ramIdx16->data());
                const u64 base = (r.sel == kSelVB) ? s_blobOffVb : s_blobOff[r.sel];
                src = ram + (r.off - base);
                ok = true;
            } else if (mapBase) {
                src = mapBase + r.off;
                ok = true;
            } else if (s_ioFile) {
                tmp.resize(r.size);
                ok = _fseeki64(s_ioFile, (long long)r.off, SEEK_SET) == 0
                  && fread(tmp.data(), 1, tmp.size(), s_ioFile) == tmp.size();
                src = tmp.data();
                msRead += _s.GetElapsed_ms_total();
            }
            pinnedReadBytes += r.size;
            if (!ok) {
                Msg("![VK ClPage] pinned page %u %s read failed — cluster path disabled",
                    r.page, r.sel == kSelVB ? "vb" : "ib");
                if (mapBase) { UnmapViewOfFile((void*)mapBase); CloseHandle(mapObj); CloseHandle(mapFile); }
                Destroy(); return false;
            }
            _s.Start();
            if (r.sel == kSelVB) s_vbPool->Upload(src, r.size, (VkDeviceSize)r.slot * kPageVbBytes);
            else                 s_pool[r.sel]->Upload(src, r.size, (VkDeviceSize)r.slot * kPageBytes);
            msUpload += _s.GetElapsed_ms_total();
            if (ps_r_cl_audit) {
                if (r.sel == kSelVB) s_pages[r.page].hashVb = HashBytes(src, r.size);
                else                 s_pages[r.page].hashIb = HashBytes(src, r.size);
            }
        }
        if (mapBase) {
            // Streaming keeps using s_ioFile; this view existed only for the install.
            // UnmapViewOfFile on a view with 336 MB resident tears down ~85k PTEs and
            // measured 40 ms -- the whole saving, spent on release work the level is
            // waiting for. Hand it to a detached thread: it owns nothing else.
            std::thread([mapBase, mapObj, mapFile]() {
                UnmapViewOfFile((void*)mapBase);
                CloseHandle(mapObj);
                CloseHandle(mapFile);
            }).detach();
            mapBase = nullptr;
        }

        if (ps_r_cl_audit) {
            // AuditPagePayload validates a page's indices AGAINST its own vertex
            // payload, so it needs both halves at once — which the file-ordered
            // install deliberately no longer holds. Re-read the pair per page.
            // r_cl_audit only: a debug run may pay for a second pass.
            xr_vector<u8> ib, vb;
            for (u32 p = 1; p < nPages; ++p) {
                PageState& ps = s_pages[p];
                if (!(ps.flags & kPagePinned) || !ps.byteSize) continue;
                const u32 t = PageType(ps);
                const u32 vbB = (s_vbPool && ps.vbByteSize) ? ps.vbByteSize : 0;
                ib.resize(ps.byteSize);
                vb.resize(vbB);
                auto grab = [&](u64 off, void* dst, u32 size, const u8* ram, u64 ramBase) {
                    if (!size) return true;
                    if (haveRam) { memcpy(dst, ram + (off - ramBase), size); return true; }
                    return s_ioFile && _fseeki64(s_ioFile, (long long)off, SEEK_SET) == 0
                        && fread(dst, 1, size, s_ioFile) == size;
                };
                const u8* ramIb = t ? (const u8*)ramIdx32->data() : (const u8*)ramIdx16->data();
                if (!grab(s_blobOff[t] + ps.byteOff, ib.data(), ps.byteSize, ramIb, s_blobOff[t])) continue;
                if (vbB && !grab(s_blobOffVb + ps.vbByteOff, vb.data(), vbB, haveRam ? (const u8*)ramVb->data() : nullptr, s_blobOffVb)) continue;
                AuditPagePayload(p, t, ib.data(), ps.byteSize, vbB ? vb.data() : nullptr, vbB);
            }
        }
        s_pages[0].resident = 1;
        // ⭐ 0-byte pages have nothing to stream — but a NON-PINNED one could
        // never become resident (want() skips empty pages, the installer never
        // sees them), leaving every cohort that contains one PERMANENTLY dead:
        // its entries never draw and a degenerate coarse ancestor covers them
        // forever (the white-polygon hole; the request chase reports it as the
        // persistent "stuck N" echo). Mark them resident up front — their
        // slot-base pair stays 0 and their entries carry index_count 0 or
        // identity addressing, so nothing ever fetches through them.
        {
            u32 empties = 0, vbOnly = 0;
            for (u32 p = 1; p < nPages; ++p) {
                if (s_pages[p].byteSize || s_pages[p].resident) continue;
                if (s_pages[p].vbByteSize) { ++vbOnly; continue; }   // impossible by construction — do NOT fake residency
                s_pages[p].resident = 1; ++empties;
            }
            if (empties || vbOnly)
                Msg("[VK ClPage] empty-page fix: %u 0-byte non-pinned pages marked resident%s",
                    empties, vbOnly ? " (+VB-ONLY pages exist — bake anomaly!)" : "");
        }
        // Remaining slots -> free lists (immediately usable).
        for (u32 t = 0; t < 3; ++t)
            for (u32 s = next[t]; s < s_slotCount[t]; ++s)
                s_freeSlots[t].push_back({ s, 0u });
        Msg("[VK ClPage] streaming ON: budget %llu MB -> slots %u+%u ib + %u vb = %llu MB pools (pinned %u+%u), pages %u (%.0f MB IB + %.0f MB VB on disk)",
            (unsigned long long)(budget >> 20), s_slotCount[0], s_slotCount[1], s_slotCount[kSelVB],
            (unsigned long long)(((u64)(s_slotCount[0] + s_slotCount[1]) * kPageBytes + (u64)s_slotCount[kSelVB] * kPageVbBytes) >> 20),
            pinnedCount[0], pinnedCount[1],
            nPages - 1, double(blobBytes[0] + blobBytes[1]) / (1024.0 * 1024.0), double(vbBlobBytes) / (1024.0 * 1024.0));
        Msg("[load step]   ClusterStream::CreatePools: poolAlloc %.0f | pinned install %u pages: read %.0f + upload %.0f ms (%u MB, source=%s) | rest %.0f ms",
            msAlloc, nPinnedInstalled, msRead, msUpload, (u32)(pinnedReadBytes >> 20),
            haveRam ? "RAM" : (ps_r_clpage_map ? "mapped file, 16 warm threads" : "file"),
            _cp.GetElapsed_ms_total() - msRead - msUpload);
    }

    // ---- Slice 2 vertex pool, EAGER mode (r_clpage 0) -------------------------
    // Full vertex blob resident in one device buffer laid out exactly like the
    // blob; each page's VB base ([2p+1] of the slot-base pairs) is its constant
    // blob offset in kVbStride units. Streaming mode uses the VB SLOT pool
    // created above instead — bases flip per install like the IB side.
    // Uploads are chunked: a single ~1.6 GB staging alloc is risky.
    if (!s_streaming && vbBlobBytes) {
        constexpr u64 kChunk = 64ull << 20;
        s_vbPool = xr_new<CVulkanBuffer>();
        s_vbPool->Create(vbBlobBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                       | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
        if (!s_vbPool->IsValid())
            { Msg("![VK ClPage] vb pool alloc failed (%llu MB)", (unsigned long long)(vbBlobBytes >> 20)); Destroy(); return false; }
        if (haveRam) {
            for (u64 off = 0; off < vbBlobBytes; off += kChunk)
                s_vbPool->Upload(ramVb->data() + off, _min(vbBlobBytes - off, kChunk), off);
        } else {
            xr_vector<u8> tmp;
            for (u64 off = 0; off < vbBlobBytes; off += kChunk) {
                const u64 sz = _min(vbBlobBytes - off, kChunk);
                tmp.resize((size_t)sz);
                const bool ok = s_ioFile
                    && _fseeki64(s_ioFile, (long long)(s_blobOffVb + off), SEEK_SET) == 0
                    && fread(tmp.data(), 1, (size_t)sz, s_ioFile) == (size_t)sz;
                if (!ok) { Msg("![VK ClPage] vb blob read failed — cluster path disabled"); Destroy(); return false; }
                s_vbPool->Upload(tmp.data(), sz, off);
            }
        }
        for (u32 p = 1; p < nPages; ++p)
            if (pages[p].flags & kPageHasVB)
                s_slotBaseHost[2 * p + 1] = pages[p].vbByteOff / kVbStride;
        Msg("[VK ClPage] vertex pool resident: %.0f MB (increment (a) — eager until VB slots stream)",
            double(vbBlobBytes) / (1024.0 * 1024.0));
    }
    return true;
}

// ============================================================================
// Runtime — phase 2: tables + state + feedback (after the final meta exists)
// ============================================================================
bool BuildState(const xr_vector<GpuMeshMeta>& finalMeta)
{
    VK::Vram::Scope _vram_scope("Clusters");
    s_total = (u32)finalMeta.size();
    if (s_total == 0 || s_pages.empty()) { Destroy(); return false; }
    const u32 nPages = (u32)s_pages.size();

    // Sub-timers (`[load step]`): this is 80 ms of the post-visual budget on
    // pripyat_full and it is five different walks over 830k entries — the split
    // decides which one is worth attacking.
    CTimer _s; _s.Start();
    float msKeys = 0, msFill = 0, msPages = 0, msCycle = 0, msLive = 0, msBufs = 0;

    // ---- Tables from the FINAL meta (duplicated shared-slice entries included) ----
    s_entryPage.resize(s_total);
    s_entryMember.assign(s_total, kNone);
    s_entryChild.assign(s_total, kNone);

    std::unordered_map<u64, u32> gid;
    gid.reserve(s_total / 4);
    for (u32 i = 0; i < s_total; ++i) {
        s_entryPage[i] = finalMeta[i]._pad1 < nPages ? finalMeta[i]._pad1 : 0;
        if (finalMeta[i].parentError < kErrInf) {
            const u64 k = KeySE(finalMeta[i].lodParent, finalMeta[i].parentError);
            auto ins = gid.emplace(k, (u32)gid.size());
            s_entryMember[i] = ins.first->second;
        }
    }
    s_groups.clear();
    s_groups.resize(gid.size());
    for (u32 i = 0; i < s_total; ++i) {
        if (finalMeta[i].selfError > 0.f) {
            auto it = gid.find(KeySE(finalMeta[i].lodSelf, finalMeta[i].selfError));
            if (it != gid.end()) s_entryChild[i] = it->second;
        }
    }
    // ⭐ Break liveness SELF-LOOPS (root cause of the 16-07 stuck-web): stall
    // chains in clusterlod can emit entries whose (lodSelf,selfError) equals
    // their own (lodParent,parentError) BITWISE — the entry is then a parent
    // of its OWN member group, making groupLive(G) require groupLive(G):
    // permanently dead by construction, unfixable by any page install (the
    // resync correctly kept it dead — the cycle is in the DATA). Such entries
    // never pass the base cut anyway (sp == pp), so dropping their child link
    // is behavior-neutral; their group's liveness then rests on real parents.
    u32 selfLoops = 0;
    for (u32 i = 0; i < s_total; ++i)
        if (s_entryChild[i] != kNone && s_entryChild[i] == s_entryMember[i]) { s_entryChild[i] = kNone; ++selfLoops; }
    msKeys = _s.GetElapsed_ms_total();
    _s.Start();
    for (u32 i = 0; i < s_total; ++i) {
        if (s_entryMember[i] != kNone) {
            GroupState& G = s_groups[s_entryMember[i]];
            G.members.push_back(i);
            G.pages.push_back(s_entryPage[i]);
        }
        if (s_entryChild[i] != kNone)
            s_groups[s_entryChild[i]].parents.push_back(i);
    }
    for (GroupState& G : s_groups) {
        std::sort(G.pages.begin(), G.pages.end());
        G.pages.erase(std::unique(G.pages.begin(), G.pages.end()), G.pages.end());
    }
    msFill = _s.GetElapsed_ms_total();
    _s.Start();
    for (u32 i = 0; i < s_total; ++i) {
        s_pages[s_entryPage[i]].entries.push_back(i);
        if (s_entryMember[i] != kNone) s_pages[s_entryPage[i]].groups.push_back(s_entryMember[i]);
    }
    for (u32 p = 0; p < nPages; ++p) {
        auto& g = s_pages[p].groups;
        std::sort(g.begin(), g.end());
        g.erase(std::unique(g.begin(), g.end()), g.end());
    }
    msPages = _s.GetElapsed_ms_total();
    _s.Start();

    if (selfLoops)
        Msg("[VK ClPage] cut %u self-loop entries (degenerate clusterlod stall chains)", selfLoops);

    {   // Cycle diagnostic BEYOND self-loops (dependency edge: G -> member group
        // of each parent). A multi-group cycle would deadlock liveness the same
        // way and needs SCC condensation — log loudly if the data ever has one.
        xr_vector<u8> color(s_groups.size(), 0);   // 0 white, 1 gray, 2 black
        struct DfsFrame { u32 g, pi; };
        xr_vector<DfsFrame> st;
        u32 backEdges = 0;
        for (u32 g0 = 0; g0 < (u32)s_groups.size(); ++g0) {
            if (color[g0]) continue;
            color[g0] = 1; st.push_back({ g0, 0 });
            while (!st.empty()) {
                DfsFrame f = st.back();
                const auto& par = s_groups[f.g].parents;
                bool pushed = false;
                for (u32 pi = f.pi; pi < (u32)par.size(); ++pi) {
                    const u32 t = s_entryMember[par[pi]];
                    if (t == kNone || color[t] == 2) continue;
                    if (color[t] == 1) { ++backEdges; continue; }
                    color[t] = 1; st.back().pi = pi + 1; st.push_back({ t, 0 }); pushed = true; break;
                }
                if (!pushed) { color[f.g] = 2; st.pop_back(); }
            }
        }
        if (backEdges)
            Msg("![VK ClPage] %u liveness cycle edges beyond self-loops — needs SCC condensation (expect a small stuck-request floor)", backEdges);
    }

    // ---- ⭐ ORPHAN COHORTS (white-polygon root cause, found 17-07) ---------------
    // Demand is parent-driven: a page is only requested when a drawn entry
    // carries the leaf bit for the group that needs it. Groups NO entry refines
    // into (parents=0) have no such path — and the self-loop cut above orphans
    // THOUSANDS of stall-chain groups this way (force-loading them all was
    // tried 17-07 and blew past the entire slot pool: 5693-page backlog, +0
    // installs, starved). The real fix is DIRECT DEMAND in world_cull.comp: a
    // non-drawable entry the cut needs requests ITSELF (top bit of the request
    // word), and the decode below fetches its member cohort — orphan pages then
    // stream in and out through the normal LRU like everything else. The count
    // is logged here so the scale of the anomaly stays visible.
    {
        u32 orphanG = 0;
        for (const GroupState& G : s_groups)
            if (G.parents.empty()) ++orphanG;
        if (orphanG)
            Msg("[VK ClPage] %u groups have NO refiner (stall chains / merged props) — direct demand covers them", orphanG);
    }
    msCycle = _s.GetElapsed_ms_total();
    _s.Start();

    // ---- Group liveness + bits (initial full evaluation) -------------------------
    for (u32 g = 0; g < (u32)s_groups.size(); ++g) {
        GroupState& G = s_groups[g];
        G.missing = 0;
        for (u32 p : G.pages) if (!s_pages[p].resident) ++G.missing;
        G.live = 0;
        WlPush(g);
    }
    s_bitsHost.assign((s_total + 15) / 16, 0u);
    PropagateLiveness();
    for (u32 i = 0; i < s_total; ++i) UpdateEntryBits(i);   // entries outside any group too
    s_groupSeen.assign(s_groups.size(), 0u);                 // want-chase epoch array
    s_tickEpoch = 0;
    s_resGen    = 0;
    msLive = _s.GetElapsed_ms_total();
    _s.Start();

    // ---- GPU state buffers -------------------------------------------------------
    const u32 bitsWords = (u32)s_bitsHost.size();
    s_touchedWords = (nPages + 31) / 32;
    const VkBufferUsageFlags stateUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkBufferUsageFlags fbUsage    = stateUsage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const u32 sbWords = (u32)s_slotBaseHost.size();   // 2 per page (IB base, VB base)
    s_bits = xr_new<CVulkanBuffer>();
    s_bits->Create((VkDeviceSize)bitsWords * 4, stateUsage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_slotBase = xr_new<CVulkanBuffer>();
    s_slotBase->Create((VkDeviceSize)sbWords * 4, stateUsage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_requests = xr_new<CVulkanBuffer>();
    s_requests->Create((VkDeviceSize)(1 + kMaxRequests) * 4, fbUsage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_touched = xr_new<CVulkanBuffer>();
    s_touched->Create((VkDeviceSize)s_touchedWords * 4, fbUsage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    if (!s_bits->IsValid() || !s_slotBase->IsValid() || !s_requests->IsValid() || !s_touched->IsValid())
        { Msg("![VK ClPage] state buffer alloc failed"); Destroy(); return false; }
    s_bits->Upload(s_bitsHost.data(), (VkDeviceSize)bitsWords * 4);
    s_slotBase->Upload(s_slotBaseHost.data(), (VkDeviceSize)sbWords * 4);
    {   // requests/touched start zeroed
        xr_vector<u32> z(_max(1u + kMaxRequests, s_touchedWords), 0u);
        s_requests->Upload(z.data(), (VkDeviceSize)(1 + kMaxRequests) * 4);
        s_touched->Upload(z.data(), (VkDeviceSize)s_touchedWords * 4);
    }
    s_stateDirty = false;

    if (s_streaming) {
        constexpr u32 F = CVulkanCommandManager::FRAMES_IN_FLIGHT;
        s_readbackStride = ((VkDeviceSize)(1 + kMaxRequests + s_touchedWords) * 4 + 255) & ~255ull;
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = s_readbackStride * F;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo ai{};
            if (VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bci, &aci, &s_readback, &s_readbackAlloc, &ai) != VK_SUCCESS
                || !ai.pMappedData)
                { Msg("![VK ClPage] readback alloc failed"); Destroy(); return false; }
            s_readbackPtr = (u8*)ai.pMappedData;
            memset(s_readbackPtr, 0, (size_t)(s_readbackStride * F));
        }
        if (ps_r_cl_audit) {
            // GPU slot audit: one resident page copied back per frame, re-hashed
            // a full FIF cycle later against its install-time hash. Non-fatal.
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = (VkDeviceSize)F * (kPageBytes + kPageVbBytes);
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo ai{};
            if (VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bci, &aci, &s_auditBuf, &s_auditAlloc, &ai) == VK_SUCCESS && ai.pMappedData)
                s_auditPtr = (u8*)ai.pMappedData;
            else
                Msg("![VK ClPage] audit buffer alloc failed — GPU slot audit disabled");
        }
        s_stagePages = xr_new<CVulkanBuffer>();
        s_stagePages->Create((VkDeviceSize)F * kMaxInstallsTick * (kPageBytes + kPageVbBytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        s_stageState = xr_new<CVulkanBuffer>();
        s_stageState->Create((VkDeviceSize)F * (bitsWords + sbWords) * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        if (!s_stagePages->IsValid() || !s_stageState->IsValid()
            || !s_stagePages->Map() || !s_stageState->Map())
            { Msg("![VK ClPage] staging alloc failed"); Destroy(); return false; }
        s_ioRun = true;
        s_ioThread = std::thread(IoThreadMain);
    }

    s_statInstalled = s_statEvicted = s_statStarved = s_statReqs = 0;
    s_active = true;
    msBufs = _s.GetElapsed_ms_total();
    Msg("[load step]   ClusterStream::BuildState: keys %.0f (%u entries -> %u groups) | groupFill %.0f | pageFill %.0f | cycleDiag %.0f | liveness %.0f | buffers %.0f ms",
        msKeys, s_total, (u32)s_groups.size(), msFill, msPages, msCycle, msLive, msBufs);
    return true;
}

void Destroy()
{
    if (s_ioRun) {
        { std::lock_guard<std::mutex> lk(s_ioMutex); s_ioRun = false; }
        s_ioCv.notify_all();
        if (s_ioThread.joinable()) s_ioThread.join();
    }
    if (s_ioFile) { fclose(s_ioFile); s_ioFile = nullptr; }
    s_ioQueue.clear(); s_ioDone.clear(); s_doneBacklog.clear(); s_ops.clear(); s_wl.clear();
    DestroyBuffers();
    s_pages.clear(); s_groups.clear(); s_groupSeen.clear(); s_tickEpoch = 0; s_resGen = 0;
    s_entryPage.clear(); s_entryMember.clear(); s_entryChild.clear();
    s_bitsHost.clear(); s_slotBaseHost.clear();
    s_freeSlots[0].clear(); s_freeSlots[1].clear(); s_freeSlots[2].clear();
    s_slotCount[0] = s_slotCount[1] = s_slotCount[2] = 0;
    s_blobOffVb = 0;
    s_active = s_streaming = false; s_total = 0; s_stateDirty = false;
    s_filePath[0] = 0;
}

bool Active() { return s_active; }

VkBuffer PoolIB16()       { return s_pool[0]  ? s_pool[0]->GetHandle()  : VK_NULL_HANDLE; }
VkBuffer PoolIB32()       { return s_pool[1]  ? s_pool[1]->GetHandle()  : VK_NULL_HANDLE; }
VkBuffer PoolVB()         { return s_vbPool   ? s_vbPool->GetHandle()   : VK_NULL_HANDLE; }
VkBuffer BitsBuffer()     { return s_bits     ? s_bits->GetHandle()     : VK_NULL_HANDLE; }
VkBuffer SlotBaseBuffer() { return s_slotBase ? s_slotBase->GetHandle() : VK_NULL_HANDLE; }
VkBuffer RequestBuffer()  { return s_requests ? s_requests->GetHandle() : VK_NULL_HANDLE; }
VkBuffer TouchedBuffer()  { return s_touched  ? s_touched->GetHandle()  : VK_NULL_HANDLE; }
const u32* HostBits()     { return s_active   ? s_bitsHost.data()       : nullptr; }
u32 ResidencyGen()        { return s_resGen; }
const u32* HostSlotBase(u32& count)
{
    count = (u32)s_slotBaseHost.size();
    return s_active && count ? s_slotBaseHost.data() : nullptr;
}
void DebugPageInfo(u32 page, u32& resident, u32& slot, u32& vbSlot)
{
    resident = 0; slot = kNone; vbSlot = kNone;
    if (!s_active || page >= (u32)s_pages.size()) return;
    resident = s_pages[page].resident;
    slot = s_pages[page].slot;
    vbSlot = s_pages[page].vbSlot;
}

// White-polygon hunt: dump WHY an entry's cohort is broken — the member group
// (gates this entry's drawable bit) and the child group (its leaf bit), with
// every missing page's full state. A missing page with bytes=0 pinned=0 is the
// permanently-unrequestable class; queued=0 with bytes>0 is a lost request.
void DebugEntryChain(u32 e)
{
    if (!s_active || e >= s_total) return;
    const u32 mg = s_entryMember[e], cg = s_entryChild[e];
    auto dumpG = [&](const char* tag, u32 g) {
        if (g == kNone) { Msg("[VK ClChain]   %s group: none", tag); return; }
        const GroupState& G = s_groups[g];
        Msg("[VK ClChain]   %s group %u: live=%u missing=%u pages=%u parents=%u members=%u",
            tag, g, (u32)G.live, G.missing, (u32)G.pages.size(), (u32)G.parents.size(), (u32)G.members.size());
        u32 shownP = 0;
        for (u32 p : G.pages) {
            const PageState& ps = s_pages[p];
            if (ps.resident) continue;
            if (shownP++ < 8)
                Msg("![VK ClChain]     MISSING page %u: bytes=%u vb=%u pinned=%u queued=%u slot=%d",
                    p, ps.byteSize, ps.vbByteSize, (ps.flags & kPagePinned) ? 1u : 0u, (u32)ps.queued, (int)ps.slot);
        }
    };
    Msg("[VK ClChain] entry %u page %u (res=%u): member=%d child=%d",
        e, s_entryPage[e], (u32)s_pages[s_entryPage[e]].resident,
        mg == kNone ? -1 : (int)mg, cg == kNone ? -1 : (int)cg);
    dumpG("member", mg);
    dumpG("child", cg);
}

// ============================================================================
// Per-frame tick (CRender::Begin, before the frame cmd opens)
// ============================================================================
void Frame()
{
    VK::Vram::Scope _vram_scope("Clusters");
    if (!s_active || !s_streaming) return;
    VK_CPU_PROBE("ClPage");
    const u32 frame = CommandManager.GetCurrentFrame();
    const u32 now   = RV().frameIndex;

    // ---- 1) Decode the fence-proven feedback slot (requests + touched) ----------
    xr_vector<std::pair<u32, u32>> wants;   // (page, urgency)
    {
        vmaInvalidateAllocation(VulkanHW.m_Allocator, s_readbackAlloc,
                                s_readbackStride * frame, s_readbackStride);
        const u32* base = (const u32*)(s_readbackPtr + s_readbackStride * frame);
        const u32* req  = base;
        const u32* tch  = base + 1 + kMaxRequests;
        const u32 cnt = _min(req[0], kMaxRequests);
        s_statReqs += cnt;
        auto want = [&](u32 p, u32 urg) {
            if (p == 0 || p >= (u32)s_pages.size()) return;
            PageState& ps = s_pages[p];
            if (ps.resident || ps.queued || !ps.byteSize) return;
            for (auto& w : wants) if (w.first == p) { w.second = _max(w.second, urg); return; }
            wants.emplace_back(p, urg);
        };
        // Transitive want-chase: a leaf's child group may be non-live because a
        // production-sibling parent sits in a cohort broken HIGHER up — and the
        // missing page there can be OFF-FRUSTUM (no GPU request will ever name
        // it). Walk the broken liveness chain upward (DAG depth <= 12, epoch
        // dedup) so every dependency of the requested group gets fetched —
        // otherwise requests spin forever at one-LOD-coarser (seen in the
        // 16-07 log: reqs ~14k/tick with +0 installs while standing).
        ++s_tickEpoch;
        xr_vector<u32> stack;
        auto chase = [&](u32 g0, u32 urg) {
            stack.clear(); stack.push_back(g0);
            while (!stack.empty()) {
                const u32 g = stack.back(); stack.pop_back();
                if (g == kNone || g >= (u32)s_groups.size()) continue;
                if (s_groupSeen[g] == s_tickEpoch) continue;
                s_groupSeen[g] = s_tickEpoch;
                GroupState& G = s_groups[g];
                if (G.live) continue;
                for (u32 p : G.pages) want(p, urg);            // own missing pages
                for (u32 q : G.parents) {
                    if (EntryLive(q)) continue;
                    want(s_entryPage[q], urg);                 // dead parent's page
                    stack.push_back(s_entryMember[q]);         // and/or its broken cohort
                }
            }
        };
        for (u32 i = 0; i < cnt; ++i) {
            const u32 v = req[1 + i];
            const u32 e = v & 0xFFFFFFu, urg = (v >> 24) & 0x7Fu;
            if (e >= s_total) continue;
            // DIRECT request (top bit): a NON-drawable entry the cut needs asked
            // for its OWN cohort — the only demand path for orphan groups (no
            // refiner exists to carry their leaf bit; see the BuildState log).
            if (v & 0x80000000u) {
                want(s_entryPage[e], urg);
                const u32 mg = s_entryMember[e];
                if (mg != kNone && mg < (u32)s_groups.size() && s_groupSeen[mg] != s_tickEpoch) {
                    ++s_statUniq;
                    chase(mg, urg);
                }
                continue;
            }
            const u32 cg = s_entryChild[e];
            if (cg == kNone || cg >= (u32)s_groups.size()) continue;
            if (s_groupSeen[cg] == s_tickEpoch) continue;   // already chased this tick
            ++s_statUniq;
            const size_t before = wants.size();
            chase(cg, urg);
            // Diagnostic: a DEAD child group whose transitive chase yields ZERO
            // wants means the CPU liveness state can't be satisfied by any page
            // install — that's a state-machine desync, not readback echo.
            if (!s_groups[cg].live && wants.size() == before) {
                ++s_statStuck;
                if (s_statStuckG == kNone) s_statStuckG = cg;
            }
        }
        for (u32 w = 0; w < s_touchedWords; ++w) {
            u32 bitsW = tch[w];
            for (u32 bit = 0; bitsW; ++bit, bitsW >>= 1)
                if (bitsW & 1u) {
                    const u32 p = w * 32 + bit;
                    if (p < (u32)s_pages.size()) s_pages[p].lastTouch = now;
                }
        }
    }

    // ---- 2) Drain IO completions into the install backlog -----------------------
    {
        std::lock_guard<std::mutex> lk(s_ioMutex);
        for (IoDone& d : s_ioDone) {
            if (d.data.empty()) { s_pages[d.page].queued = 0; Msg("![VK ClPage] page %u read failed", d.page); continue; }
            s_doneBacklog.push_back(std::move(d));
        }
        s_ioDone.clear();
    }

    // ---- 3) Install (slot allocation + staged copies for RecordFrameOps) --------
    u32 installed = 0;
    for (size_t i = 0; i < s_doneBacklog.size() && installed < kMaxInstallsTick && s_ops.size() < kMaxInstallsTick;) {
        IoDone& d = s_doneBacklog[i];
        PageState& ps = s_pages[d.page];
        const u32 t = PageType(ps);
        const u32 vbBytes = s_vbPool ? ps.vbByteSize : 0;   // must match the IO thread's layout
        u32 slot = AllocSlot(t);
        if (slot == kNone) {
            EvictOne(t);   // frees a slot for a FUTURE tick (retire delay)
            ++i; continue;
        }
        u32 vbSlot = kNone;
        if (vbBytes) {
            vbSlot = AllocSlot(kSelVB);
            if (vbSlot == kNone) {
                s_freeSlots[t].push_back({ slot, 0u });   // untouched this frame — immediately reusable
                EvictOne(kSelVB);
                ++i; continue;
            }
        }
        ps.slot = slot; ps.vbSlot = vbSlot; ps.queued = 0; ps.lastTouch = now;
        if (ps_r_cl_audit) {
            ps.hashIb = HashBytes(d.data.data(), ps.byteSize);
            ps.hashVb = vbBytes ? HashBytes(d.data.data() + ps.byteSize, vbBytes) : 0;
            AuditPagePayload(d.page, t, d.data.data(), ps.byteSize,
                             vbBytes ? d.data.data() + ps.byteSize : nullptr, vbBytes);
        }
        s_slotBaseHost[2 * d.page] = slot * IdxPerSlot(t);
        if (vbBytes) s_slotBaseHost[2 * d.page + 1] = vbSlot * kVbSlotVerts;
        FlipPage(d.page, true);
        s_stateDirty = true;
        s_ops.push_back({ t, (VkDeviceSize)slot * kPageBytes,
                          vbBytes ? (VkDeviceSize)vbSlot * kPageVbBytes : 0,
                          ps.byteSize, vbBytes, std::move(d.data) });
        s_doneBacklog[i] = std::move(s_doneBacklog.back()); s_doneBacklog.pop_back();
        ++installed; ++s_statInstalled;
    }
    PropagateLiveness();

    // ---- 4) Schedule new reads (priority = request urgency) ---------------------
    if (!wants.empty()) {
        // DESCENDING here: when wants overflow the in-flight cap, the STRONGEST
        // requests must go first (the first deployed build pushed weakest-first).
        std::sort(wants.begin(), wants.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::lock_guard<std::mutex> lk(s_ioMutex);
        for (auto& w : wants) {
            if (s_ioQueue.size() >= kMaxIoInflight) break;
            s_pages[w.first].queued = 1;
            s_pages[w.first].queuedFrame = now;
            s_ioQueue.push_back({ w.first, w.second });
        }
        // The worker pops from the BACK — keep the queue ascending by urgency.
        std::sort(s_ioQueue.begin(), s_ioQueue.end(), [](const IoJob& a, const IoJob& b) { return a.urgency < b.urgency; });
        s_ioCv.notify_all();
    }

    // ---- 5) Keep the free lists fed while demand outstrips them -----------------
    {
        u32 demand[3] = { 0, 0, 0 };
        for (const IoDone& d : s_doneBacklog) {
            ++demand[PageType(s_pages[d.page])];
            if (s_vbPool && s_pages[d.page].vbByteSize) ++demand[kSelVB];
        }
        for (u32 t = 0; t < 3; ++t) {
            u32 free_ = (u32)s_freeSlots[t].size();
            for (u32 k = free_; k < _min(demand[t], 8u); ++k)
                if (!EvictOne(t)) break;
        }
    }

    // ---- Stats (throttled) -------------------------------------------------------
    if (RV().dwTimeGlobal > s_statLogLast + 3000) {
        s_statLogLast = RV().dwTimeGlobal;
        const bool loud = ps_r_profiler > 0;

        // Queued-leak watchdog: a page flagged `queued` must pass through the IO
        // thread within moments; ~15s without delivery = a leaked flag silently
        // blocking every future want() of that page — re-arm it and say so.
        u32 leaked = 0;
        for (u32 p = 1; p < (u32)s_pages.size(); ++p) {
            PageState& ps = s_pages[p];
            if (ps.queued && !ps.resident && now > ps.queuedFrame + 900) { ps.queued = 0; ++leaked; }
        }


        // Deep forensics for the stuck web BEFORE the resync heals it: walk the
        // dead closure and classify — a group with real missing pages (chase/IO
        // failed to fetch) vs an all-satisfied dead web (propagation failed).
        if (loud && s_statStuck && s_statStuckG != kNone && s_statStuckG < (u32)s_groups.size()) {
            xr_vector<u32> st; st.push_back(s_statStuckG);
            std::unordered_map<u32, u8> seen; seen.emplace(s_statStuckG, 1);
            u32 deadN = 0, satisfied = 0, firstMissG = kNone, firstMissN = 0, missQueued = 0;
            while (!st.empty() && seen.size() < 4096) {
                const u32 g = st.back(); st.pop_back();
                const GroupState& G = s_groups[g];
                if (G.live) continue;
                ++deadN;
                u32 rm = 0;
                for (u32 p : G.pages)
                    if (!s_pages[p].resident) { ++rm; if (s_pages[p].queued) ++missQueued; }
                if (rm) { if (firstMissG == kNone) { firstMissG = g; firstMissN = rm; } }
                else ++satisfied;
                for (u32 q : G.parents) {
                    if (EntryLive(q)) continue;
                    const u32 gq = s_entryMember[q];
                    if (gq != kNone && seen.emplace(gq, 1).second) st.push_back(gq);
                }
            }
            Msg("[VK ClPage]   stuck G=%u: closure dead=%u satisfied=%u | firstRealMiss G=%d (%u pages, %u of misses queued)",
                s_statStuckG, deadN, satisfied, firstMissG == kNone ? -1 : (int)firstMissG, firstMissN, missQueued);
        }

        // SELF-HEALING resync: recount missing + full liveness fixpoint. Any
        // correction is the desync being healed — the counts name the bug class.
        // Runs regardless of the profiler (the game must converge either way).
        u32 fixedMiss = 0, liveFlips = 0;
        FullResync(fixedMiss, liveFlips);
        if (fixedMiss || liveFlips || leaked)
            Msg("[VK ClPage]   resync: %u miss-counters fixed, %u live flips, %u queued leaks re-armed",
                fixedMiss, liveFlips, leaked);

        // White-polygon hunt: force a FULL bits+slotBase re-upload every stats
        // tick (~240 KB / 3 s). The CPU draw-contract validator only sees the
        // HOST mirror — if the GPU copy ever diverged (an upload recorded into
        // a cmd that never reached the queue), nothing else would resync it.
        // A hole that now heals within ~3 s fingers exactly that class.
        s_stateDirty = true;

        // ---- DRAW-CONTRACT VALIDATOR (white-polygon desync hunt) ----------------
        // Every entry the cull will DRAW (drawable bit0) reads its vertices from
        // pageSlotBase[2*page+1] + first_vertex. This checks — on the ACTUAL CPU
        // state that RecordFrameOps uploads — that each drawable entry's page is
        // resident with a VB base that matches its live VB slot (repacked) or 0
        // (raw). A violation here IS the garbage-vertex bug, surviving the resync.
        if (ps_r_cl_audit) {
            u32 badRes = 0, badVbSlot = 0, staleBase = 0, rawNonzero = 0, staleIb = 0, badSlot = 0, shown = 0;
            for (u32 e = 0; e < s_total; ++e) {
                const u32 word = e >> 4u, shift = (e & 15u) * 2u;
                if (((s_bitsHost[word] >> shift) & 1u) == 0u) continue;   // not drawable
                const u32 p = s_entryPage[e];
                if (p >= (u32)s_pages.size()) continue;
                const PageState& ps = s_pages[p];
                const bool hasVB = s_vbPool && ps.vbByteSize > 0;
                if (!ps.resident) {
                    ++badRes;
                    if (shown++ < 10) Msg("![VK ClVB-RT] drawable entry %u page %u NOT resident (slot=%u vbSlot=%u)", e, p, ps.slot, ps.vbSlot);
                    continue;
                }
                // IB base [2p]: firstIndex = pageSlotBase[2p] + ib_first. A stale IB
                // base after slot reuse points indices into ANOTHER page's slot →
                // wrong vertices → stretched polygon. Pinned/eager pages have a
                // fixed base and no live slot — skip the slot-derived check for them.
                if (ps.byteSize > 0) {
                    if (ps.slot == kNone) {
                        ++badSlot;
                        if (shown++ < 10) Msg("![VK ClVB-RT] drawable entry %u page %u resident but slot=NONE (has IB payload)", e, p);
                    } else {
                        const u32 wantIb = ps.slot * IdxPerSlot(PageType(ps));
                        if (s_slotBaseHost[2 * p] != wantIb) {
                            ++staleIb;
                            if (shown++ < 10) Msg("![VK ClVB-RT] drawable entry %u page %u STALE IB base: host=%u want=%u (slot=%u type=%u)",
                                                  e, p, s_slotBaseHost[2 * p], wantIb, ps.slot, PageType(ps));
                        }
                    }
                }
                if (hasVB) {
                    if (ps.vbSlot == kNone) {
                        ++badVbSlot;
                        if (shown++ < 10) Msg("![VK ClVB-RT] drawable entry %u page %u resident but vbSlot=NONE (has VB payload)", e, p);
                    } else {
                        const u32 want = ps.vbSlot * kVbSlotVerts;
                        if (s_slotBaseHost[2 * p + 1] != want) {
                            ++staleBase;
                            if (shown++ < 10) Msg("![VK ClVB-RT] drawable entry %u page %u STALE VB base: host=%u want=%u (vbSlot=%u)",
                                                  e, p, s_slotBaseHost[2 * p + 1], want, ps.vbSlot);
                        }
                    }
                } else if (s_slotBaseHost[2 * p + 1] != 0u) {
                    ++rawNonzero;
                    if (shown++ < 10) Msg("![VK ClVB-RT] drawable entry %u page %u is RAW but VB base=%u (should be 0)",
                                          e, p, s_slotBaseHost[2 * p + 1]);
                }
            }
            if (badRes || badVbSlot || staleBase || rawNonzero || staleIb || badSlot)
                Msg("![VK ClVB-RT] ^^ draw-contract violations: notResident=%u noSlot=%u staleIB=%u noVbSlot=%u staleVB=%u rawNonzero=%u (THIS is the bug)",
                    badRes, badSlot, staleIb, badVbSlot, staleBase, rawNonzero);
            else
                Msg("[VK ClVB-RT] draw-contract OK: all drawable entries have resident pages + matching IB/VB bases");
            Msg("[VK ClPage] gpu-audit: %u slots verified, %u MISMATCHED | payload-bad pages %u",
                s_auditOk, s_auditBad, s_payloadBad);
        }

        if (loud) {
            u32 res = 0; u64 resB = 0;
            for (u32 p = 1; p < (u32)s_pages.size(); ++p)
                if (s_pages[p].resident) { ++res; resB += s_pages[p].byteSize + (s_vbPool ? s_pages[p].vbByteSize : 0u); }
            u32 ioq; { std::lock_guard<std::mutex> lk(s_ioMutex); ioq = (u32)s_ioQueue.size(); }
            Msg("[VK ClPage] resident %u/%u pages (%.0f MB), IOq %u backlog %u | +%u inst -%u evict, starved %u, reqs %u (uniq %u, stuck %u)",
                res, (u32)s_pages.size() - 1, double(resB) / (1024.0 * 1024.0), ioq, (u32)s_doneBacklog.size(),
                s_statInstalled, s_statEvicted, s_statStarved, s_statReqs, s_statUniq, s_statStuck);
        }
        s_statInstalled = s_statEvicted = s_statStarved = s_statReqs = 0;
        s_statUniq = s_statStuck = 0; s_statStuckG = kNone;
    }
}

// ============================================================================
// Frame-cmd recording (before any world/VSM cull dispatch)
// ============================================================================
void RecordFrameOps(VkCommandBuffer cmd, u32 frameSlot)
{
    if (!s_active || !s_streaming || cmd == VK_NULL_HANDLE) return;
    if (frameSlot >= CVulkanCommandManager::FRAMES_IN_FLIGHT) return;
    VK_CPU_PROBE("ClPage/record");

    auto barrier = [&](VkPipelineStageFlags ss, VkAccessFlags sa,
                       VkPipelineStageFlags ds, VkAccessFlags da) {
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = sa; mb.dstAccessMask = da;
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 1, &mb, 0, nullptr, 0, nullptr);
    };

    // ---- GPU slot audit: verify the copy-back this frame slot recorded a full
    // FIF cycle ago (its fence is proven waited before re-recording). The slot
    // bytes on the GPU must re-hash to exactly what the install staged.
    if (s_auditPtr) {
        AuditPending& ap = s_auditPend[frameSlot];
        if (ap.page != kNone && ap.page < (u32)s_pages.size()) {
            const PageState& ps = s_pages[ap.page];
            // Same payload still resident in the SAME slots? (an evict+reinstall
            // between record and verify would read another page's slot bytes)
            if (ps.resident && ps.slot == ap.slot && ps.vbSlot == ap.vbSlot
                && ps.hashIb == ap.hIb && ps.hashVb == ap.hVb) {
                const VkDeviceSize base = (VkDeviceSize)frameSlot * (kPageBytes + kPageVbBytes);
                vmaInvalidateAllocation(VulkanHW.m_Allocator, s_auditAlloc, base, kPageBytes + kPageVbBytes);
                const u8* rb = s_auditPtr + (size_t)base;
                const bool okIb = !ap.ibBytes || HashBytes(rb, ap.ibBytes) == ap.hIb;
                const bool okVb = !ap.vbBytes || HashBytes(rb + kPageBytes, ap.vbBytes) == ap.hVb;
                if (okIb && okVb) ++s_auditOk;
                else {
                    ++s_auditBad;
                    Msg("![VK ClPage-GPU] page %u (slot %u vbSlot %u, ib %u vb %u bytes): GPU BYTES != installed payload (ib %s, vb %s) — slot data corruption CONFIRMED",
                        ap.page, ps.slot, ps.vbSlot, ap.ibBytes, ap.vbBytes,
                        okIb ? "ok" : "BAD", okVb ? "ok" : "BAD");
                }
            }
            ap.page = kNone;
        }
    }

    // ---- Feedback resolve: copy LAST frames' request/touched data out, reset ----
    barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    {
        const VkDeviceSize dst = s_readbackStride * frameSlot;
        VkBufferCopy r0{ 0, dst, (VkDeviceSize)(1 + kMaxRequests) * 4 };
        vkCmdCopyBuffer(cmd, s_requests->GetHandle(), s_readback, 1, &r0);
        VkBufferCopy r1{ 0, dst + (VkDeviceSize)(1 + kMaxRequests) * 4, (VkDeviceSize)s_touchedWords * 4 };
        vkCmdCopyBuffer(cmd, s_touched->GetHandle(), s_readback, 1, &r1);
    }
    barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, s_requests->GetHandle(), 0, 4, 0u);   // reset the append counter
    vkCmdFillBuffer(cmd, s_touched->GetHandle(), 0, VK_WHOLE_SIZE, 0u);

    // ---- Page-data installs + dirty state upload ---------------------------------
    // WAR guard: prior frames' index fetches / compute reads of the pools + state
    // (TRANSFER src stage: the audit copy-back also reads the pools).
    barrier(VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (!s_ops.empty()) {
        constexpr VkDeviceSize kOpStride = kPageBytes + kPageVbBytes;   // staging layout: [IB half | VB half]
        const VkDeviceSize frameBase = (VkDeviceSize)frameSlot * kMaxInstallsTick * kOpStride;
        u8* stage = (u8*)s_stagePages->m_Mapped + (size_t)frameBase;
        u32 n = 0;
        for (InstallOp& op : s_ops) {
            memcpy(stage + (size_t)n * kOpStride, op.data.data(), op.ibBytes);
            VkBufferCopy c{ frameBase + (VkDeviceSize)n * kOpStride, op.dstOff, op.ibBytes };
            vkCmdCopyBuffer(cmd, s_stagePages->GetHandle(), s_pool[op.type]->GetHandle(), 1, &c);
            if (op.vbBytes) {
                memcpy(stage + (size_t)n * kOpStride + kPageBytes, op.data.data() + op.ibBytes, op.vbBytes);
                VkBufferCopy cv{ frameBase + (VkDeviceSize)n * kOpStride + kPageBytes, op.vbDstOff, op.vbBytes };
                vkCmdCopyBuffer(cmd, s_stagePages->GetHandle(), s_vbPool->GetHandle(), 1, &cv);
            }
            ++n;
        }
        s_stagePages->Flush();
        s_ops.clear();
    }
    if (s_stateDirty) {
        const u32 bitsWords = (u32)s_bitsHost.size();
        const u32 nPages    = (u32)s_slotBaseHost.size();
        u8* stage = (u8*)s_stageState->m_Mapped + (size_t)frameSlot * (bitsWords + nPages) * 4;
        memcpy(stage, s_bitsHost.data(), (size_t)bitsWords * 4);
        memcpy(stage + (size_t)bitsWords * 4, s_slotBaseHost.data(), (size_t)nPages * 4);
        s_stageState->Flush();
        const VkDeviceSize srcBase = (VkDeviceSize)frameSlot * (bitsWords + nPages) * 4;
        VkBufferCopy c0{ srcBase, 0, (VkDeviceSize)bitsWords * 4 };
        vkCmdCopyBuffer(cmd, s_stageState->GetHandle(), s_bits->GetHandle(), 1, &c0);
        VkBufferCopy c1{ srcBase + (VkDeviceSize)bitsWords * 4, 0, (VkDeviceSize)nPages * 4 };
        vkCmdCopyBuffer(cmd, s_stageState->GetHandle(), s_slotBase->GetHandle(), 1, &c1);
        s_stateDirty = false;
    }

    // ---- GPU slot audit: schedule the next copy-back (one resident page/frame).
    // Recorded AFTER the installs so a page installed this very frame reads its
    // post-install bytes (transfer→transfer barrier orders the copies).
    if (s_auditPtr && s_pages.size() > 1) {
        const u32 nP = (u32)s_pages.size();
        u32 pick = kNone;
        for (u32 step = 0; step < nP - 1; ++step) {
            const u32 p = 1 + (s_auditCursor - 1 + step) % (nP - 1);
            const PageState& ps = s_pages[p];
            if (!ps.resident || !ps.byteSize || ps.slot == kNone) continue;
            pick = p; s_auditCursor = p + 1; break;
        }
        if (pick != kNone) {
            const PageState& ps = s_pages[pick];
            const u32 vbB = (s_vbPool && ps.vbSlot != kNone) ? ps.vbByteSize : 0;
            barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            const VkDeviceSize dstBase = (VkDeviceSize)frameSlot * (kPageBytes + kPageVbBytes);
            VkBufferCopy a{ (VkDeviceSize)ps.slot * kPageBytes, dstBase, ps.byteSize };
            vkCmdCopyBuffer(cmd, s_pool[PageType(ps)]->GetHandle(), s_auditBuf, 1, &a);
            if (vbB) {
                VkBufferCopy av{ (VkDeviceSize)ps.vbSlot * kPageVbBytes, dstBase + kPageBytes, vbB };
                vkCmdCopyBuffer(cmd, s_vbPool->GetHandle(), s_auditBuf, 1, &av);
            }
            s_auditPend[frameSlot] = { pick, ps.byteSize, vbB, ps.slot, ps.vbSlot, ps.hashIb, ps.hashVb };
        }
    }

    barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

}} // namespace VK::ClusterStream
