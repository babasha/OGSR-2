// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// Global render profiler — see vk_profiler.h for the design overview.

#include "stdafx.h"
#include "vk_profiler.h"
#include "HW_Vulkan.h"           // VulkanHW (device / physdev / allocator / instance)
#include "vk_command_buffer.h"   // VK_FRAMES_IN_FLIGHT

#include "../../xr_3da/device.h"           // Device.fTimeDeltaRealMS / dwTimeGlobal
#include "../../xr_3da/IGame_Persistent.h" // g_pGamePersistent (rain/wet state on MARK)
#include "../../xr_3da/Environment.h"      // CEnvironment::wetness_factor / CurrentEnv
#include "../../xr_3da/Stats.h"            // Device.Statistic — main-thread CPU attribution

#include <algorithm>                       // std::sort — top CPU zones in MaybeLog, frame-time percentiles
#include <cmath>                           // std::ceil — nearest-rank percentile index
#include <mutex>                           // checkpoint intern table (workers + main thread)
#include <vector>                          // checkpoint post-mortem query

// r_profiler console var (declared/registered in vk_console_min.cpp): 0 off
// logging, 1 = periodic [VK Perf] log, 2 = + overlay (Phase 2). Default 1.
extern int ps_r_profiler;
#ifdef __SWITCH__
extern "C" double xr_nx_gpu2_perf_window(void);   // CRender_Vulkan.cpp NxGpu2 (nfs13)
#endif

namespace VK { namespace Prof {

namespace {

// ---- debug-utils entry points (loaded once; null = unsupported → no-ops) ----
PFN_vkSetDebugUtilsObjectNameEXT  pfnSetName   = nullptr;
PFN_vkCmdBeginDebugUtilsLabelEXT  pfnBeginLbl  = nullptr;
PFN_vkCmdEndDebugUtilsLabelEXT    pfnEndLbl    = nullptr;

// ---- NV diagnostic checkpoints (null = unsupported → no-ops) ----
PFN_vkCmdSetCheckpointNV       pfnSetChk = nullptr;
PFN_vkGetQueueCheckpointDataNV pfnGetChk = nullptr;

// Marker strings must stay valid until the driver is queried after a device
// loss, so checkpoint names are interned into this fixed arena. Uploads record
// from seqParallel workers while the main thread records the frame → the
// lookup is mutex-guarded (a handful of calls per frame; cost is noise).
constexpr u32 kChkMax = 96;
char        s_chkNames[kChkMax][40];
u32         s_chkCount = 0;
std::mutex  s_chkLock;

const char* InternChk(const char* name)
{
    std::lock_guard<std::mutex> lock(s_chkLock);
    for (u32 i = 0; i < s_chkCount; ++i)
        if (0 == strcmp(s_chkNames[i], name)) return s_chkNames[i];
    if (s_chkCount >= kChkMax) return s_chkNames[0];   // arena full: reuse slot 0 (better than nothing)
    xr_strcpy(s_chkNames[s_chkCount], name);
    return s_chkNames[s_chkCount++];
}

bool  s_inited      = false;

// ---- GPU timestamp query pool ----
constexpr u32 kSlots       = VK_FRAMES_IN_FLIGHT;
constexpr u32 kQueriesSlot = kMaxZones * 2;             // begin+end per zone
VkQueryPool   s_pool       = VK_NULL_HANDLE;
float         s_periodNs   = 0.f;                       // timestampPeriod (ns/tick)

// ---- per-slot bookkeeping ----
u32  s_zoneOwner = 0;              // nfs13: thread that ran this frame's FrameBegin (see ZonesOwnThread)
u32  s_curSlot   = 0;
u32  s_curZoneN  = 0;                 // zones opened this frame
u32  s_zoneDepth = 0;                 // current open-zone nesting depth (0 while between top-level passes)
bool s_slotWritten[kSlots] = {};
u32  s_slotZoneN [kSlots]  = {};

// ---- persistent per-zone stats, slot INTERNED BY NAME ----
// The index used to be the zone's open order, on the assumption that the order
// is stable frame to frame. IT IS NOT: conditional passes (VolSmoke, Shadow/Far,
// Glass, VSM/*, World/Compose*) skip whole frames, and a pass that skips shifts
// every LATER zone down one index. The history ring lives in the slot, so
// "World/Color" would quietly accumulate "World/Skinned" samples and vice versa.
// Observed 2026-07-23: an r_preskin A/B straddled a VolSmoke transition and read
// World/Color=0.00(0.00/0.00) next to World/Skinned=4.60 while their parent
// World zone was unchanged at 8.99 vs 9.00 -- a 6x "win" that was pure
// misattribution. Any per-zone A/B taken before this fix is suspect.
//
// Fix: the stat slot is interned by name (stable for the process lifetime) while
// the QUERY index stays the contiguous open order, so every query in [0, nz) is
// still written and reset every frame and we never have to reason about query
// availability. s_slotMap remembers, per in-flight slot, which stat slot each
// query index belonged to when that frame was recorded.
ZoneStat s_zone[kMaxZones]   = {};
u32      s_zoneCount         = 0;     // number of INTERNED zones (never shrinks)
u8       s_slotMap[kSlots][kMaxZones] = {};   // [in-flight slot][query idx] -> stat slot
u64 s_cpuStart[kMaxZones]{};

// ---- counters (reset each frame; snapshot kept for getters) ----
FrameInfo s_frameLast = {};
u32 s_cDraws=0, s_cInst=0, s_cTris=0, s_cPipe=0, s_cDesc=0;

// ---- VRAM ----
MemSnap s_mem = {};
u32     s_deviceLocalHeapMask = 0;

// ---- logging ----
u32  s_lastLogMs = 0;
bool s_markReq   = false;

// XROS_PRECACHE_HITCH=1 -- trace every frame of the precache window (the tail of
// 'st_client_synchronising'), both the CPU phase split and the GPU zone dump.
static const bool s_precacheTrace = []() {
    const char* e = std::getenv("XROS_PRECACHE_HITCH");
    return e && atoi(e) != 0;
}();

// ---- CPU phase meters (see CpuPhaseIdx in the header) ----
u64   s_phaseT0[CPU_PHASE_COUNT]{};
float s_phaseMs[CPU_PHASE_COUNT]{};

double QpcMs(u64 a, u64 b)
{
    const u64 freq = CPU::QPCFreq();
    if (freq == 0) return 0.0;
    return double(b - a) * 1000.0 / double(freq);
}

void FoldSample(ZoneStat& z, float gpuMs)
{
    z.gpuLast = gpuMs;
    z.ring[z.ringHead] = gpuMs;
    z.ringHead = (z.ringHead + 1) % kHistory;
    // recompute avg/min/max over the ring (cheap; kHistory is small)
    float sum = 0.f, mn = 1e9f, mx = 0.f; u32 n = 0;
    for (u32 i = 0; i < kHistory; ++i) {
        float v = z.ring[i];
        if (v <= 0.f) continue;
        sum += v; n++;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    z.gpuAvg = n ? sum / n : gpuMs;
    z.gpuMin = n ? mn : gpuMs;
    z.gpuMax = n ? mx : gpuMs;
}

void QueryMemory()
{
    if (!VulkanHW.m_Allocator) return;
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(VulkanHW.m_Allocator, budgets);
    u64 used = 0, budget = 0; u32 allocs = 0, blocks = 0;
    for (u32 h = 0; h < VK_MAX_MEMORY_HEAPS; ++h) {
        if (!(s_deviceLocalHeapMask & (1u << h))) continue;
        used   += budgets[h].usage;
        budget += budgets[h].budget;
        allocs += budgets[h].statistics.allocationCount;
        blocks += budgets[h].statistics.blockCount;
    }
    s_mem.usedBytes   = used;
    s_mem.budgetBytes = budget;
    s_mem.allocCount  = allocs;
    s_mem.blockCount  = blocks;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Phase 0 — debug-utils
// ---------------------------------------------------------------------------
void Init()
{
    if (s_inited) return;
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;   // not ready yet; retried next frame
    s_inited = true;

    pfnSetName  = (PFN_vkSetDebugUtilsObjectNameEXT) vkGetDeviceProcAddr(VulkanHW.m_Device, "vkSetDebugUtilsObjectNameEXT");
    pfnBeginLbl = (PFN_vkCmdBeginDebugUtilsLabelEXT) vkGetDeviceProcAddr(VulkanHW.m_Device, "vkCmdBeginDebugUtilsLabelEXT");
    pfnEndLbl   = (PFN_vkCmdEndDebugUtilsLabelEXT)   vkGetDeviceProcAddr(VulkanHW.m_Device, "vkCmdEndDebugUtilsLabelEXT");

    if (VulkanHW.m_bCheckpointsSupported) {
        pfnSetChk = (PFN_vkCmdSetCheckpointNV)       vkGetDeviceProcAddr(VulkanHW.m_Device, "vkCmdSetCheckpointNV");
        pfnGetChk = (PFN_vkGetQueueCheckpointDataNV) vkGetDeviceProcAddr(VulkanHW.m_Device, "vkGetQueueCheckpointDataNV");
        Msg("[VK Prof] NV checkpoints %s", (pfnSetChk && pfnGetChk) ? "armed" : "proc-addr FAILED");
    }

    // device-local heap mask for VRAM accounting
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(VulkanHW.m_PhysicalDevice, &mp);
    for (u32 h = 0; h < mp.memoryHeapCount; ++h)
        if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            s_deviceLocalHeapMask |= (1u << h);

    // timestamp query pool
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(VulkanHW.m_PhysicalDevice, &props);
    s_periodNs = props.limits.timestampPeriod;
    if (s_periodNs > 0.f) {
        VkQueryPoolCreateInfo qci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = kQueriesSlot * kSlots;
        if (vkCreateQueryPool(VulkanHW.m_Device, &qci, nullptr, &s_pool) != VK_SUCCESS)
            s_pool = VK_NULL_HANDLE;
    }
    Msg("[VK Perf] profiler init (timestamps=%s, period=%.2f ns)",
        s_pool ? "on" : "off", s_periodNs);
}

void SetObjectName(VkObjectType type, uint64_t handle, const char* name)
{
    if (!s_inited) Init();   // names are often set at resource-creation (pre-frame)
    if (!pfnSetName || !handle || !name) return;
    VkDebugUtilsObjectNameInfoEXT ni{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
    ni.objectType   = type;
    ni.objectHandle = handle;
    ni.pObjectName  = name;
    pfnSetName(VulkanHW.m_Device, &ni);
}

void NameImage (VkImage  img, const char* name) { SetObjectName(VK_OBJECT_TYPE_IMAGE,  (uint64_t)img, name); }
void NameBuffer(VkBuffer buf, const char* name) { SetObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)buf, name); }
void NameSet   (VkDescriptorSet s, const char* name) { SetObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET, (uint64_t)s, name); }

void CmdBeginLabel(VkCommandBuffer cmd, const char* name, float r, float g, float b)
{
    if (!pfnBeginLbl) return;
    VkDebugUtilsLabelEXT lbl{ VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
    lbl.pLabelName = name;
    lbl.color[0] = r; lbl.color[1] = g; lbl.color[2] = b; lbl.color[3] = 1.f;
    pfnBeginLbl(cmd, &lbl);
}

void CmdEndLabel(VkCommandBuffer cmd)
{
    if (pfnEndLbl) pfnEndLbl(cmd);
}

// Receiver for the pure.h seqFrame hook: fold each member's wall time into a
// "seq:<class>" CPU probe (prints in the 5s [VK CPUprobes] line). Members under
// 0.02 ms are skipped so the 48 probe slots go to the real spenders.
static void SeqFrameProbeCb(const char* typeName, float ms)
{
    if (ms < 0.02f) return;
    const char* p = typeName ? typeName : "?";
    if      (0 == strncmp(p, "class ",  6)) p += 6;
    else if (0 == strncmp(p, "struct ", 7)) p += 7;
    char nm[32];
    _snprintf(nm, sizeof(nm) - 1, "seq:%s", p);
    nm[sizeof(nm) - 1] = 0;
    const int slot = CpuProbeSlot(nm);
    if (slot >= 0) CpuProbeAdd(slot, ms);
}

// ---------------------------------------------------------------------------
// Phase 1 — per-frame zones
// ---------------------------------------------------------------------------
void FrameBegin(VkCommandBuffer cmd, u32 frameIndex)
{
    Init();
    { LoadProf::Scope _lpC("pb.chk"); Checkpoint(cmd, "FrameBegin"); }   // marks even zone-less frames (loading screens)
    s_zoneOwner = GetCurrentThreadId();   // nfs13: the recording thread of THIS frame owns the zone table
    s_curSlot  = frameIndex % kSlots;
    s_curZoneN = 0;
    s_zoneDepth = 0;   // defensive: rebalance in case a zone span was left open last frame

    // Ask the engine to gather CStatTimer stats (fills [VK CPU] in MaybeLog) even
    // without rs_stats — the rs_stats overlay needs fonts the VK build never creates.
    // device.cpp reads this next frame; the ~µs QPC cost is noise.
    g_bForceStatGather = (ps_r_profiler > 0);
    // Per-member seqFrame attribution (pure.h hook) → "seq:<class>" CPU probes.
    // Diagnoses EngineTOTAL growing while every engine sub-timer reads 0 (the
    // "fps decays after flying, GPU underloaded" pattern — WHO in seqFrame grew).
    g_seq_profile_hook = (ps_r_profiler > 0) ? &SeqFrameProbeCb : nullptr;

    // snapshot + reset CPU-side counters for getters
    s_frameLast.draws     = s_cDraws;     s_cDraws = 0;
    s_frameLast.instances = s_cInst;      s_cInst  = 0;
    s_frameLast.tris      = s_cTris;      s_cTris  = 0;
    s_frameLast.pipeBinds = s_cPipe;      s_cPipe  = 0;
    s_frameLast.descBinds = s_cDesc;      s_cDesc  = 0;

    // read back the PREVIOUS occupant of this slot (its fence already proved the
    // GPU finished that submission), fold into the per-zone history.
    if (s_pool && s_slotWritten[s_curSlot]) {
        const u32 base = s_curSlot * kQueriesSlot;
        const u32 nz   = s_slotZoneN[s_curSlot];
        u64 q[kQueriesSlot] = {};
        // nfs13: a frame that recorded NO zone must clear gpuLast as well -- that is how the table froze for a whole
        // run (every zone rejected as foreign, nz = 0, the last folded frame reported forever).
        if (!nz)
            for (u32 i = 0; i < s_zoneCount; ++i) s_zone[i].gpuLast = 0.f;
        LoadProf::Scope _lpQ("pb.queries"); // [rt prof] the GPU timestamps of this slot's last frame
        if (nz && vkGetQueryPoolResults(VulkanHW.m_Device, s_pool, base, nz * 2,
                                        sizeof(u64) * nz * 2, q, sizeof(u64),
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            // Fold each query pair into the stat slot it belonged to WHEN THAT
            // FRAME WAS RECORDED — not into the current frame's open order.
            // A flat flag array, not a bitmask: kMaxZones outgrew 64 and a `u64`
            // would have silently stopped clearing every zone past the 64th.
            bool folded[kMaxZones] = {};
            for (u32 z = 0; z < nz; ++z) {
                const u32   stat = s_slotMap[s_curSlot][z];
                const float ms   = float(double(q[z * 2 + 1] - q[z * 2]) * s_periodNs * 1e-6);
                if (ms >= 0.f && ms < 1000.f) { FoldSample(s_zone[stat], ms); folded[stat] = true; }
            }
            // A pass that did not run in that frame must stop contributing its
            // stale gpuLast to gpu_total (its history is left intact).
            for (u32 i = 0; i < s_zoneCount; ++i)
                if (!folded[i]) s_zone[i].gpuLast = 0.f;
        }
    }

    // reset this slot's range (must be outside a render pass — we are)
    if (s_pool)
    { LoadProf::Scope _lpR("pb.reset"); vkCmdResetQueryPool(cmd, s_pool, s_curSlot * kQueriesSlot, kQueriesSlot); }

    { LoadProf::Scope _lpM("pb.mem"); QueryMemory(); }
}

// ---------------------------------------------------------------------------
// NV diagnostic checkpoints
// ---------------------------------------------------------------------------
void Checkpoint(VkCommandBuffer cmd, const char* name)
{
    if (!pfnSetChk || cmd == VK_NULL_HANDLE) return;
    pfnSetChk(cmd, InternChk(name));
}

void DumpCheckpoints(const char* why)
{
    static bool s_dumped = false;   // one dump per device loss is enough (SL floods errors after)
    if (s_dumped) return;
    s_dumped = true;

    if (!pfnGetChk) { Msg("![VK Chk] (%s) NV checkpoints unsupported — no GPU post-mortem", why); return; }

    struct { const char* name; VkQueue q; } queues[] = {
        { "graphics", VulkanHW.m_GraphicsQueue },
        { "transfer", VulkanHW.m_TransferQueue },
        { "compute",  VulkanHW.m_ComputeQueue  },
    };
    Msg("![VK Chk] ===== GPU post-mortem (%s) — hang is between COMPLETED and STARTED =====", why);
    for (u32 qi = 0; qi < 3; ++qi) {
        if (queues[qi].q == VK_NULL_HANDLE) continue;
        bool dup = false;   // families often alias to one VkQueue — dump each handle once
        for (u32 j = 0; j < qi; ++j) if (queues[j].q == queues[qi].q) { dup = true; break; }
        if (dup) continue;

        u32 n = 0;
        pfnGetChk(queues[qi].q, &n, nullptr);
        if (n == 0) { Msg("![VK Chk] %s queue: no checkpoint data", queues[qi].name); continue; }
        std::vector<VkCheckpointDataNV> data(n);
        for (auto& d : data) { d.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV; d.pNext = nullptr; }
        pfnGetChk(queues[qi].q, &n, data.data());
        for (u32 i = 0; i < n; ++i) {
            const char* marker = (const char*)data[i].pCheckpointMarker;
            // Only our interned pointers are trustworthy strings.
            const bool ours = marker >= &s_chkNames[0][0] && marker < &s_chkNames[0][0] + sizeof(s_chkNames);
            const char* when = (data[i].stage == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) ? "COMPLETED"
                             : (data[i].stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)    ? "STARTED  "
                                                                                       : "stage?   ";
            if (ours) Msg("![VK Chk] %s queue: last %s = '%s'", queues[qi].name, when, marker);
            else      Msg("![VK Chk] %s queue: last %s = %p (foreign)", queues[qi].name, when, marker);
        }
    }
    Msg("![VK Chk] ===== end post-mortem =====");
}

// Stable stat slot for a zone name. Linear scan: kMaxZones is 64 and this runs
// once per pass per frame, so it is far cheaper than the misattribution it ends.
// ⚠Returns -1 when the table is full. It used to `return kMaxZones - 1` — which
// does not drop the overflowing zone, it MERGES it into whichever zone happened
// to intern last, and reports the sum under that zone's name. That is invisible:
// the reader sees a plausible number next to a familiar label. It cost a whole
// r_vsm_meshlet A/B on 2026-08-13 (Bins/VoxCull landing in Bins/Meshlet's slot,
// so the flag looked inert when it was the METER that was lying). Refusing the
// zone loses one measurement; aliasing it corrupts another one silently, and a
// missing zone is at least visibly missing.
static int InternZone(const char* name)
{
    for (u32 i = 0; i < s_zoneCount; ++i)
        if (0 == strcmp(s_zone[i].name, name)) return (int)i;
    if (s_zoneCount >= kMaxZones) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true; Msg("![VK Prof] zone table full (%u) - '%s' and later zones are UNMEASURED (raise kMaxZones)", kMaxZones, name); }
        return -1;
    }
    const u32 i = s_zoneCount++;
    xr_strcpy(s_zone[i].name, name);
    return (int)i;
}

// ⚠ЗОНЫ -- ТОЛЬКО ГЛАВНЫЙ ПОТОК (19-09-2026). Счётчики зон (s_curZoneN,
// s_zoneDepth, s_slotMap, s_cpuStart) -- обычные глобалы, а с выносом записи
// команд на воркеры (r_mt_record 2) тела пассов зовут ZoneBegin со своих
// потоков. Два потока в этой таблице -- это не «неточные числа», а слот запроса,
// выданный дважды, и таймстемпы, приписанные чужой зоне.
// Поэтому с чужого потока остаётся только МЕТКА отладчика (она потоко-безопасна
// и по-прежнему рисует пасс в RenderDoc), а таблица не трогается. Цена записи
// таких пассов видна прибором [VK RecCPU], который меряет циклы, а не через
// эти зоны.
// ⚠Первый вызов запоминает поток: инициализация рендера идёт там же, где кадр.
static u32 s_zoneThread = 0;

static inline bool ZonesOwnThread()
{
    // nfs13: the owner is the thread that ran THIS frame's FrameBegin, not the first one that ever opened a zone. With
    // the render thread the gameplay frame is recorded there, while the load (and the first zones) ran on
    // the main thread: latching that one rejected every zone of the real frames. Worker lanes (r_mt_record 2) are
    // still foreign and get a label only.
    const u32 tid = GetCurrentThreadId();
    if (s_zoneOwner) return s_zoneOwner == tid;
    if (!s_zoneThread) s_zoneThread = tid;
    return s_zoneThread == tid;
}

int ZoneBegin(VkCommandBuffer cmd, const char* name)
{
    Checkpoint(cmd, name);   // GPU progress marker (post-mortem on device loss)

    if (!ZonesOwnThread()) { CmdBeginLabel(cmd, name); return -1; }

    const int q = (int)s_curZoneN;                                     // query index = open order
    if (q >= (int)kMaxZones) { CmdBeginLabel(cmd, name); return -1; }  // overflow: label only

    const int statI = InternZone(name);                                // stats index = by name
    if (statI < 0) { CmdBeginLabel(cmd, name); return -1; }            // table full: label only, never aliased onto another zone
    const u32 stat = (u32)statI;
    s_slotMap[s_curSlot][q] = (u8)stat;
    s_zone[stat].depth = s_zoneDepth++;   // 0 = top-level pass; nested children are excluded from gpu_total
    s_cpuStart[q] = CPU::QPC();

    if (s_pool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_pool,
                            s_curSlot * kQueriesSlot + q * 2);
    CmdBeginLabel(cmd, name);

    s_curZoneN++;
    return q;
}

void ZoneEnd(VkCommandBuffer cmd, int zone)
{
    if (zone < 0) { CmdEndLabel(cmd); return; }   // overflow zone (label only; never bumped s_zoneDepth)
    if (s_zoneDepth) s_zoneDepth--;
    CmdEndLabel(cmd);
    if (s_pool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_pool,
                            s_curSlot * kQueriesSlot + zone * 2 + 1);
    const u64 now = CPU::QPC();
    s_zone[s_slotMap[s_curSlot][zone]].cpuLast = (float)QpcMs(s_cpuStart[zone], now);
}

// CPU probes (see header): named per-frame wall-time accumulators for main-
// thread code the pass zones can't see. Harvested + reset in FrameEnd.
namespace {
    constexpr u32 kMaxProbes = 48;
    struct Probe { char name[32]; float accum; float last; };
    Probe s_probes[kMaxProbes];
    u32   s_probeCount = 0;
}

void FrameEnd(VkCommandBuffer cmd)
{
    Checkpoint(cmd, "FrameEnd");   // a hang AFTER the last pass (present chain) still shows

    // A pass that did not run THIS frame must not report last time's cpuLast:
    // [VK CPUzones] is read next frame (MaybeLog runs right after FrameBegin), so
    // a stale value would show a cadence-gated pass (spot tiles, VSM, Glass) as if
    // it had recorded commands. That is how "children cost more CPU than their
    // parent" showed up in the 23-07 logs. Same discipline as the gpuLast reset in
    // FrameBegin — see the s_slotMap note at the top of this file.
    {
        bool ran[kMaxZones] = {};   // flags, not a u64 — see the folded[] note in FrameBegin
        for (u32 q = 0; q < s_curZoneN && q < kMaxZones; ++q) ran[s_slotMap[s_curSlot][q]] = true;
        for (u32 i = 0; i < s_zoneCount; ++i)
            if (!ran[i]) s_zone[i].cpuLast = 0.f;
    }

    s_slotWritten[s_curSlot] = true;
    s_slotZoneN [s_curSlot]  = s_curZoneN;

    // Harvest this frame's CPU probes (streamers/queue-rebuilds/near-set walks).
    for (u32 i = 0; i < s_probeCount; ++i) {
        s_probes[i].last  = s_probes[i].accum;
        s_probes[i].accum = 0.f;
    }
}

void CpuPhaseBegin(u32 idx)
{
    if (idx < CPU_PHASE_COUNT) s_phaseT0[idx] = CPU::QPC();
}

void CpuPhaseEnd(u32 idx)
{
    if (idx >= CPU_PHASE_COUNT) return;
    s_phaseMs[idx] = (float)QpcMs(s_phaseT0[idx], CPU::QPC());
}

// ---------------------------------------------------------------------------
// CPU probes API (statics live above FrameEnd, which harvests them).
// ---------------------------------------------------------------------------
int CpuProbeSlot(const char* name)
{
    for (u32 i = 0; i < s_probeCount; ++i)
        if (0 == strcmp(s_probes[i].name, name))
            return (int)i;
    // ⚠A silent -1 makes the probe a no-op and its block reads as FREE. That is
    // the exact failure this hunt kept hitting from the other side (a meter that
    // answers a question nobody asked and says nothing about it), so say so.
    if (s_probeCount >= kMaxProbes) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true; Msg("![VK Prof] CPU probe slots exhausted (%u) — '%s' and later probes read 0, NOT free", kMaxProbes, name); }
        return -1;
    }
    xr_strcpy(s_probes[s_probeCount].name, name);
    s_probes[s_probeCount].accum = s_probes[s_probeCount].last = 0.f;
    return (int)s_probeCount++;
}

void CpuProbeAdd(int slot, float ms)
{
    if (slot >= 0 && slot < (int)s_probeCount)
        s_probes[slot].accum += ms;
}

CpuProbeScope::CpuProbeScope(int s) : slot(s)
{
    t0 = CPU::QPC();
}

CpuProbeScope::~CpuProbeScope()
{
    CpuProbeAdd(slot, (float)QpcMs(t0, CPU::QPC()));
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
void RequestMark() { s_markReq = true; }

// Main-thread CPU attribution for the frame that just ended, as text. Shared by
// the 5-second line and the per-hitch dump — they differ ONLY in how much they
// are willing to print, and that difference is the whole point: a top-8 list is
// right for a periodic summary and wrong for a spike, because the spike is
// exactly what the cutoff was hiding.
static void BuildCpuZoneList(char* out, size_t cap, float minMs, u32 maxN)
{
    u32 order[kMaxZones]; u32 n = 0;
    for (u32 z = 0; z < s_zoneCount && n < kMaxZones; ++z) if (s_zone[z].cpuLast > minMs) order[n++] = z;
    std::sort(order, order + n, [](u32 a, u32 b) { return s_zone[a].cpuLast > s_zone[b].cpuLast; });
    if (n > maxN) n = maxN;
    int co = 0;
    for (u32 i = 0; i < n && co < (int)cap - 48; ++i)
        co += _snprintf(out + co, cap - co - 1, "%s=%.2f ", s_zone[order[i]].name, s_zone[order[i]].cpuLast);
    out[co > 0 ? co : 0] = 0;
}

static void BuildCpuProbeList(char* out, size_t cap, float minMs, u32 maxN)
{
    u32 order[kMaxProbes]; u32 n = 0;
    for (u32 p = 0; p < s_probeCount && n < kMaxProbes; ++p) if (s_probes[p].last > minMs) order[n++] = p;
    std::sort(order, order + n, [](u32 a, u32 b) { return s_probes[a].last > s_probes[b].last; });
    if (n > maxN) n = maxN;
    int co = 0;
    for (u32 i = 0; i < n && co < (int)cap - 48; ++i)
        co += _snprintf(out + co, cap - co - 1, "%s=%.2f ", s_probes[order[i]].name, s_probes[order[i]].last);
    out[co > 0 ? co : 0] = 0;
}

// ---------------------------------------------------------------------------
// FRAME-TIME WINDOW — the tail, sampled EVERY frame
// ---------------------------------------------------------------------------
// ⚠The cpu=/fps= fields of [VK Perf] are ONE reading of Device.fTimeDeltaRealMS
// taken at print time — one frame in ~340. That is enough for a mean across many
// logs and structurally useless for the question a virtual shadow map exists to
// answer ("does it still hitch?"): a stutter is a TAIL event, and an average that
// swallows two bad frames out of five hundred moves by hundredths of a ms.
// Cascades vs VSM is exactly that comparison — cascades pay in rare redraw SPIKES
// (SunShadow avg 9.73 / max 15.19) while VSM pays evenly, so a verdict taken on
// means is a verdict on the metric that cannot see the difference. Hence: push
// every frame here, report percentiles over the whole window.
//
// The window RESETS on each periodic print, so every line describes its own 5 s
// and consecutive lines of an A/B sweep are directly comparable. A `vk_perf` MARK
// prints the partial window WITHOUT resetting it — a manual snapshot must not
// destroy the periodic statistic it happened to land in the middle of.
//
// ⚠cpu and gpu within ONE sample are not the same frame: cpuMs is the frame that
// just ended, while the zone timings folded back in FrameBegin belong to the frame
// recorded VK_FRAMES_IN_FLIGHT earlier. Over a 5-second window the two
// DISTRIBUTIONS still describe the same interval (shifted by 3 frames), which is
// all a percentile needs — but do not pair them frame by frame.
static constexpr u32   kFrameWindow = 4096;   // 5 s up to ~820 fps; an overflow is counted and printed, never silent
static constexpr float k_hitch_ms   = 33.f;   // a doubled 60 Hz frame — the smallest thing a player calls a stutter

static float  s_winCpu[kFrameWindow];
static float  s_winGpu[kFrameWindow];
static u32    s_winN      = 0;    // samples STORED (== seen unless the window overflowed)
static u32    s_winSeen   = 0;    // frames measured this window
static u32    s_winHitch  = 0;    // frames >= k_hitch_ms
static double s_winCpuSum = 0.0, s_winGpuSum = 0.0;

static void FrameWindowPush(float cpuMs, float gpuMs)
{
    ++s_winSeen;
    s_winCpuSum += cpuMs;
    s_winGpuSum += gpuMs;
    if (cpuMs >= k_hitch_ms) ++s_winHitch;
    if (s_winN < kFrameWindow) { s_winCpu[s_winN] = cpuMs; s_winGpu[s_winN] = gpuMs; ++s_winN; }
}

static void FrameWindowReset()
{
    s_winN = s_winSeen = s_winHitch = 0;
    s_winCpuSum = s_winGpuSum = 0.0;
}

// ⚠TRUE frame period, measured here rather than taken from the engine.
// Device.fTimeDeltaRealMS is `Timer.GetElapsed_ms()` (device.cpp) — an INTEGER
// millisecond count. Every percentile drawn from it therefore lands on a whole
// millisecond, and the 1% low flips between 143 and 71 fps for no reason other
// than 7 and 14 ms being adjacent integers. At a 6 ms frame that quantisation is
// ±8%, which is larger than most of the effects this profiler exists to measure —
// and it is silently baked into every `fps=` this renderer has ever logged.
// MaybeLog runs exactly once per frame, so the gap between two consecutive calls
// IS the frame period, at QPC resolution.
// ⚠A menu, an alt-tab or a level load produces one enormous gap that is not a
// frame; those are dropped rather than reported as a 20-second stutter. The
// threshold is far above any real hitch, so genuine spikes still count.
static float FramePeriodMs()
{
    static u64 s_prev{};
    const u64 now = CPU::QPC();
    const float ms = s_prev ? float(QpcMs(s_prev, now)) : 0.f;
    s_prev = now;
    return (ms > 0.f && ms < 500.f) ? ms : 0.f;   // 0 = "no usable sample this frame"
}

// Nearest-rank percentile over an ALREADY SORTED array: idx = ceil(p·n) - 1.
// Nearest-rank rather than interpolated on purpose — at n≈500 the p99 IS the 5th
// worst frame, and naming a frame time that actually occurred is more useful when
// chasing a stutter than a blend of two that didn't.
static float Pct(const float* sorted, u32 n, float p)
{
    if (!n) return 0.f;
    int idx = int(std::ceil(double(p) * double(n))) - 1;
    if (idx < 0) idx = 0;
    if (idx >= (int)n) idx = (int)n - 1;
    return sorted[idx];
}

// ⭐⭐The tail, not the average. The 5-second line samples ONE frame in ~340 and
// structurally cannot land on the rare frame anyone cares about: of 238 samples
// in the 31-07 log exactly FOUR fell on a 40 ms frame — and those four accidents
// are the only reason we know the spike is CPU-side (record=42 ms) and lives
// inside Pass_World (World=40 ms) while its own children sum to 6 ms. Four
// lucky samples is not a measurement, so print the same attribution for EVERY
// frame that hitched.
//
// ⚠Runs at the TOP of the next frame, which is precisely when the data is
// valid: FrameEnd already harvested that frame's probes into .last and reset
// the zone cpuLast of passes that did not run. Reading it any later would mix
// two frames. Device.fTimeDeltaRealMS is likewise the frame that just ended --
// the [VK Perf] cpu= field is the same value, and it agreed with record= in all
// four accidental samples, which is what makes the pairing trustworthy here.
static void DumpHitchCpu()
{
    static u32 s_dumped = 0;             // threshold: the shared k_hitch_ms above, so the dump and the [VK Frame] hitch count can never disagree
    constexpr u32 k_max_dumps = 200;     // bounded, but the running count prints so an exhausted budget and a quiet game cannot look alike

    const float ms = Device.fTimeDeltaRealMS;
    // Loading frames are excluded because they are legitimately huge -- except when
    // those frames ARE the subject. Under XROS_PRECACHE_HITCH every precache frame
    // dumps regardless of size: it is the only per-frame split the sync phase gets.
    if (Device.dwPrecacheFrame != 0) { if (!s_precacheTrace) return; }
    else if (ms < k_hitch_ms) return;
    if (s_dumped >= k_max_dumps) return;
    ++s_dumped;

    char zl[768], pl[768];
    BuildCpuZoneList (zl, sizeof(zl), 0.10f, kMaxZones);    // EVERYTHING, no top-N
    BuildCpuProbeList(pl, sizeof(pl), 0.10f, kMaxProbes);

    Msg("~ [VKhitch] frame %u took %.0f ms (#%u) | beginWait=%.2f calc=%.2f record=%.2f present=%.2f",
        Device.dwFrame, ms, s_dumped,
        s_phaseMs[CPU_BEGIN], s_phaseMs[CPU_CALC], s_phaseMs[CPU_RECORD], s_phaseMs[CPU_END]);
    Msg("~ [VKhitch]   zones: %s", zl);
    Msg("~ [VKhitch]   probes: %s", pl);
}

void MaybeLog()
{
    // Under the precache trace every loading frame gets a full zone dump: the sync
    // phase is sixty of these frames and the 5-second window never lands inside it.
    if (s_precacheTrace && Device.dwPrecacheFrame && ps_r_profiler > 0) s_markReq = true;

    const bool mark = s_markReq;
    if (ps_r_profiler <= 0 && !mark) { s_markReq = false; return; }

    DumpHitchCpu();

    // gpu_total = wall-clock frame GPU time ≈ sum of TOP-LEVEL passes only. Nested
    // zones (World/*, Shadow/*, VSM/*) are a breakdown of their parent, so adding
    // them would double-count; they're still printed individually.
    // ⚠Summed BEFORE the string build, not inside it: that loop breaks out early
    // when the 2 KB line fills up, and with 64 interned zones it can — which used
    // to silently truncate the total as well as the text.
    float gpuTotal = 0.f;
    for (u32 z = 0; z < s_zoneCount; ++z)
        if (s_zone[z].depth == 0) gpuTotal += s_zone[z].gpuLast;

    // Every-frame sample for the tail statistics. Loading frames are legitimately
    // huge and would swamp the first window after a level load — the same
    // exclusion DumpHitchCpu makes.
    const float frameMs = FramePeriodMs();   // must be called EVERY frame: it keeps the QPC delta
    if (frameMs > 0.f && Device.dwPrecacheFrame == 0) FrameWindowPush(frameMs, gpuTotal);

    // WHY dwTimeContinual AND NOT dwTimeGlobal (24-08). The 5 s deadline used to
    // read the GAME clock, which time_factor SCALES and a pause STOPS. That made
    // the one thing an A/B in this renderer needs impossible: freezing the world.
    // With time_factor 0.001 the deadline simply never elapses, so [VK Perf] and
    // [VK Frame] stop printing altogether - a sweep run that way measures nothing
    // and does not say so. And without the freeze the SCENE drifts under the
    // sweep: wind turns, the weather script moves the sun and the fog, and the
    // Trees zone alone was measured swinging 4.6 -> 13.6 ms between frames of one
    // run, which is larger than most of the differences being looked for.
    // dwTimeContinual comes from TimerMM: real milliseconds, unscaled, unpaused -
    // the same fix the client console channel needed for the same reason.
    const u32 nowMs = Device.dwTimeContinual;
    if (!mark && nowMs < s_lastLogMs + 5000) return;
    s_lastLogMs = nowMs;
    s_markReq   = false;

    // 2048 fitted ~64 zones and kMaxZones is 128 now. Capped at 3584, not 4096:
    // Msg() formats into a string4096, so the zone list plus the cpu/fps/VRAM text
    // around it has to fit in 4096 or the tail is silently cut off.
    char line[3584]; int off = 0;
    for (u32 z = 0; z < s_zoneCount; ++z) {
        off += _snprintf(line + off, sizeof(line) - off - 1, "%s=%.2f(%.2f/%.2f) ",
                         s_zone[z].name, s_zone[z].gpuLast, s_zone[z].gpuMin, s_zone[z].gpuMax);
        if (off > (int)sizeof(line) - 64) break;
    }
    line[off > 0 ? off : 0] = 0;

    const float cpuMs = Device.fTimeDeltaRealMS;
    const float fps   = cpuMs > 0.01f ? 1000.f / cpuMs : 0.f;
    const double usedMB = double(s_mem.usedBytes)   / (1024.0 * 1024.0);
    const double budMB  = double(s_mem.budgetBytes) / (1024.0 * 1024.0);

    Msg("[VK Perf]%s cpu=%.2fms fps=%.0f gpu_total=%.2fms | %s| VRAM %.0f/%.0f MB (alloc %u/%u blk)",
        mark ? " MARK" : "", cpuMs, fps, gpuTotal, line, usedMB, budMB, s_mem.allocCount, s_mem.blockCount);

    // Tail statistics over the window — the same quantity as [VK Perf] cpu=/fps=,
    // done over every frame instead of the one that happened to be current. Read
    // p99 (and its fps twin, the "1% low") for smoothness; read hitch= for whether
    // the frame ever fell off a cliff at all.
    if (s_winN)
    {
        static float scratch[kFrameWindow];   // main thread only; 16 KB, sorted in place
        memcpy(scratch, s_winCpu, sizeof(float) * s_winN);
        std::sort(scratch, scratch + s_winN);
        const float c50 = Pct(scratch, s_winN, 0.50f), c95 = Pct(scratch, s_winN, 0.95f);
        const float c99 = Pct(scratch, s_winN, 0.99f), cMax = scratch[s_winN - 1];
        const float cAvg = float(s_winCpuSum / double(s_winSeen));

        memcpy(scratch, s_winGpu, sizeof(float) * s_winN);
        std::sort(scratch, scratch + s_winN);
        const float g95 = Pct(scratch, s_winN, 0.95f), g99 = Pct(scratch, s_winN, 0.99f);
        const float gAvg = float(s_winGpuSum / double(s_winSeen));

        char ovf[64] = {};
        if (s_winSeen > s_winN)   // never let a truncated window read like a complete one
            _snprintf(ovf, sizeof(ovf) - 1, " [window full: %u frames unsampled]", s_winSeen - s_winN);

        // jitter = p99/p50. The number that answers "is it smoother?" on its own:
        // it is scale-free, so it stays meaningful when the scene (and therefore
        // the absolute frame time) changes between two runs, and it catches the
        // failure mode averages cannot — a BIMODAL frame time, where most frames
        // are fast and every Nth costs several times more. A shadow system on a
        // cadence produces exactly that, and at 1.0 there is nothing to feel.
        const float jitter = (c50 > 0.01f) ? c99 / c50 : 0.f;

        Msg("[VK Frame] n=%u fps_avg=%.0f fps_p99=%.0f | cpu avg=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f jitter=%.2fx | gpu avg=%.2f p95=%.2f p99=%.2f | hitch>%.0fms=%u%s",
            s_winSeen,
            cAvg > 0.01f ? 1000.f / cAvg : 0.f,
            c99  > 0.01f ? 1000.f / c99  : 0.f,
            cAvg, c50, c95, c99, cMax, jitter, gAvg, g95, g99, k_hitch_ms, s_winHitch, ovf);
#ifdef __SWITCH__
        {
            // nfs13: the whole GPU frame (first..last command of the frame buffer) against the zone sum over the same
            // window -- what no zone covers: the head before ExecutePasses, the barriers/WFI between passes, the tail.
            const double whole = xr_nx_gpu2_perf_window();
            Msg("[VK Frame] gpu whole=%.2f zones=%.2f untracked=%.2f (timestamp ticks; x1.65 = real ms on GM20B)", whole, gAvg,
                whole - double(gAvg));
        }
#endif
    }
    if (!mark) FrameWindowReset();   // a MARK reports the partial window and leaves it running

    // VRAM attribution (Stage E-1): who holds the memory. Compact line with the
    // 5s dump; full sorted table at r_profiler>=3 or on an explicit mark.
    if (mark || ps_r_profiler >= 3)
        VK::Vram::LogFull();
    else
        VK::Vram::LogCompact();

    // Main-thread CPU attribution: the engine's own subsystem timers (same data as
    // rs_stats, but into the log). On vistas the frame is CPU-bound on ONE core
    // (cpu >> gpu while task-manager shows ~10% total) — this line says WHERE.
    if (Device.Statistic) {
        const CStats& S = *Device.Statistic;
        Msg("[VK CPU] engine=%.2f sched=%.2f updateCL=%.2f render=%.2f/%.2f (calc=%.2f dump=%.2f wait=%.2f) anim=%.2f phys=%.2f ai_think=%.2f ai_vis=%.2f snd=%.2f ray=%.2f | dtGame=%.4fms tf=%.3f",
            S.EngineTOTAL.result, S.Sheduler.result, S.UpdateClient.result,
            S.RenderTOTAL.result, S.RenderTOTAL_Real.result,
            S.RenderCALC.result, S.RenderDUMP.result, S.RenderDUMP_Wait.result,
            S.Animation.result, S.Physics.result, S.AI_Think.result, S.AI_Vis.result,
            S.Sound.result, S.clRAY.result, Device.fTimeDelta * 1000.f, Device.time_factor());
        Msg("[VK CPU objs] crows=%u updated=%u active=%u total=%u rays=%u", S.UpdateClient_crows, S.UpdateClient_updated, S.UpdateClient_active, S.UpdateClient_total,
            S.clRAY.count);
    }

    // ⭐СКОЛЬКО ВЫЗОВОВ СТОИТ ЗА ЗАПИСЬЮ КОМАНД. record = 1.8 мс из 5.1 мс CPU
    // (18-09-2026) — самый крупный кусок кадра, и без этих чисел непонятно,
    // много ли это: 1.8 мс на 300 вызовов и на 8000 — это разные диагнозы и
    // разные лечения (батчинг против стоимости одного вызова).
    // Счётчики уже собирались для оверлея, но в лог не попадали.
    {
        const FrameInfo fi = s_frameLast;
        Msg("[VK Draws] draws=%u inst=%u tris=%u pipeBinds=%u descBinds=%u | record=%.2f ms => %.1f us per draw",
            fi.draws, fi.instances, fi.tris, fi.pipeBinds, fi.descBinds,
            s_phaseMs[CPU_RECORD], fi.draws ? s_phaseMs[CPU_RECORD] * 1000.f / float(fi.draws) : 0.f);
    }

    // CPU phases of the render section: real work (calc/record) vs GPU-blocking
    // waits (beginWait = fence+acquire, submitPresent can block on queue/vsync).
    Msg("[VK CPUphase] beginWait=%.2f calc=%.2f record=%.2f submitPresent=%.2f (sum=%.2f)",
        s_phaseMs[CPU_BEGIN], s_phaseMs[CPU_CALC], s_phaseMs[CPU_RECORD], s_phaseMs[CPU_END],
        s_phaseMs[CPU_BEGIN] + s_phaseMs[CPU_CALC] + s_phaseMs[CPU_RECORD] + s_phaseMs[CPU_END]);

    // Renderer-side CPU: top zones by command-recording/collect time this frame
    // (ZoneBegin..ZoneEnd wall time on the render thread, NOT GPU time).
    {
        char cl[512];
        BuildCpuZoneList(cl, sizeof(cl), 0.02f, 20);   // 18-09: порог опущен — record 0.9 мс размазан тонким слоем по ~40 пассам, а не сидит в одном
        if (cl[0]) Msg("[VK CPUzones] %s", cl);
    }

    // CPU probes — helpers OUTSIDE the pass zones (streamer ticks, caster-queue
    // rebuilds, tree near-set walk, grass update...). Same top-by-ms format.
    {
        char cl[512];
        BuildCpuProbeList(cl, sizeof(cl), 0.05f, 12);
        if (cl[0]) Msg("[VK CPUprobes] %s", cl);
    }

    // On an explicit MARK, also stamp the weather/wet state so the snapshot is
    // self-describing ("was it raining / had puddles formed when FPS sat?").
    if (mark && g_pGamePersistent) {
        const auto& env = g_pGamePersistent->Environment();
        float density = (env.CurrentEnv ? env.CurrentEnv->rain_density : 0.f);
        Msg("[VK Perf] MARK  rain_density=%.2f wetness=%.2f", density, env.wetness_factor);
    }
}

// ---------------------------------------------------------------------------
// Counters / getters
// ---------------------------------------------------------------------------
void CountDraw(u32 instances, u32 tris) { s_cDraws++; s_cInst += instances; s_cTris += tris; }
void CountPipelineBind()   { s_cPipe++; }
void CountDescriptorBind() { s_cDesc++; }

u32 GetZones(const ZoneStat** outArray) { if (outArray) *outArray = s_zone; return s_zoneCount; }
MemSnap GetMem() { return s_mem; }

FrameInfo GetFrame()
{
    float gpu = 0.f;
    for (u32 z = 0; z < s_zoneCount; ++z) if (s_zone[z].depth == 0) gpu += s_zone[z].gpuLast;   // top-level only (see MaybeLog)
    s_frameLast.gpuMs = gpu;
    s_frameLast.cpuMs = Device.fTimeDeltaRealMS;
    s_frameLast.fps   = s_frameLast.cpuMs > 0.01f ? 1000.f / s_frameLast.cpuMs : 0.f;
    return s_frameLast;
}

void Shutdown()
{
    if (s_pool && VulkanHW.m_Device) {
        vkDestroyQueryPool(VulkanHW.m_Device, s_pool, nullptr);
        s_pool = VK_NULL_HANDLE;
    }
    for (bool& w : s_slotWritten) w = false;
    s_inited = false;
    pfnSetName = nullptr; pfnBeginLbl = nullptr; pfnEndLbl = nullptr;
}

}} // namespace VK::Prof
