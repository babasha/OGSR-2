// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  GLOBAL RENDER PROFILER  (vk_profiler)
// ============================================================================
//  One place that answers "where did the frame go" for the whole Vulkan
//  backend — not per-feature, global. Two layers:
//
//  PHASE 0 — debug-utils labels & object names. The EXT_debug_utils extension
//    is always enabled (g_InstanceExtensions), so we wrap every pass in a named
//    label region and tag key resources. Cost ~0; the payoff is that RenderDoc
//    and Nsight Graphics show OUR passes/resources by name and give pro-grade
//    per-draw GPU timing / flame graphs for free.
//
//  PHASE 1 — in-engine data collection:
//    • GPU timestamp ZONES (one per pass; nested-ready) with a rolling history
//      ring → last / avg / min / max ms per zone.
//    • CPU time per zone (QueryPerformanceCounter), CPU frame time, FPS.
//    • VRAM via VMA (vmaGetHeapBudgets) — used / budget / live allocations.
//    Logged to the engine log every ~5 s as "[VK Perf]" and ON DEMAND via the
//    `vk_perf` console command (a precise MARK snapshot at the instant FPS sits).
//
//  PHASE 2 (vk_profiler_overlay, ImGui) reads the GetZones()/GetMem() getters
//  below to draw the live overlay; nothing here depends on ImGui.
// ============================================================================
#pragma once
#include "stdafx.h"
#include "../../xrCore/load_prof.h" // VK_CPU_PROBE feeds [rt prof]

namespace VK { namespace Prof {

// ---------------------------------------------------------------------------
// Phase 0 — debug-utils (idempotent lazy init; all no-ops if unsupported)
// ---------------------------------------------------------------------------
void Init();   // loads the EXT_debug_utils entry points once (needs VulkanHW set up)
void SetObjectName(VkObjectType type, uint64_t handle, const char* name);
void CmdBeginLabel(VkCommandBuffer cmd, const char* name, float r = 0.6f, float g = 0.6f, float b = 0.9f);
void CmdEndLabel(VkCommandBuffer cmd);

// Typed convenience wrappers for the handles we name most.
void NameImage (VkImage  img, const char* name);
void NameBuffer(VkBuffer buf, const char* name);
void NameSet   (VkDescriptorSet s, const char* name);

// ---------------------------------------------------------------------------
// NV diagnostic checkpoints (VK_NV_device_diagnostic_checkpoints; no-ops when
// unsupported). Checkpoint() drops a named marker into the command stream —
// ZoneBegin does this automatically, call it manually only on non-zone command
// buffers (uploads, immediates). After a DEVICE_LOST, DumpCheckpoints() logs,
// per queue, the last marker the GPU STARTED and the last it COMPLETED — the
// hang lives between them. Markers are interned; names must be short.
// ---------------------------------------------------------------------------
void Checkpoint(VkCommandBuffer cmd, const char* name);
void DumpCheckpoints(const char* why);   // call on device loss / fence timeout (logs once per loss)

// ---------------------------------------------------------------------------
// Phase 1 — per-frame GPU/CPU zones
// ---------------------------------------------------------------------------
// ⚠This is a HARD ceiling, and hitting it used to corrupt attribution silently:
// InternZone saturated onto the LAST slot, so every zone opened after the 64th
// reported its time under the 64th zone's NAME. Measured 2026-08-13 — with the
// Bins/* sub-zones on, the table filled exactly, and `Bins/VoxCull` spent a whole
// A/B session masquerading as `Bins/Meshlet` (which read ~0.7 ms even with
// r_vsm_meshlet 0, because it was never Meshlet's number). Overflow is now
// refused and logged instead of aliased; 128 gives real headroom for the
// per-dispatch sub-zones at r_profiler 2.
constexpr u32 kMaxZones    = 128;  // top-level passes + sub-zones (World/*, Shadow/*, Deform, VSM/*, Bins/*) + headroom
constexpr u32 kHistory     = 96;   // samples kept per zone (avg/min/max + graph)

// Called by ExecutePasses. FrameBegin reads back the previous occupant of this
// slot (its fence already proved completion), folds it into the history, then
// resets the slot. ZoneBegin/ZoneEnd bracket one pass (GPU timestamps + CPU QPC
// + a debug label). FrameEnd closes the slot.
void FrameBegin(VkCommandBuffer cmd, u32 frameIndex);
int  ZoneBegin (VkCommandBuffer cmd, const char* name);  // returns a zone id (or -1)
void ZoneEnd   (VkCommandBuffer cmd, int zone);
void FrameEnd  (VkCommandBuffer cmd);

// Once per frame (after FrameBegin's readback): emit the [VK Perf] line if 5 s
// elapsed or a MARK was requested.
void MaybeLog();
void RequestMark();   // `vk_perf` console command → forces the next MaybeLog to print

// Coarse main-thread CPU PHASE meters (QPC, sub-ms) — separates real CPU work
// from GPU-blocking waits inside the frame's render section. Logged as
// [VK CPUphase]. Begin/End bracket one phase per frame (single-threaded).
enum CpuPhaseIdx : u32 {
    CPU_BEGIN = 0,    // CRender::Begin — fence wait + swapchain acquire (GPU/present block)
    CPU_CALC,         // CRender::Calculate — scene traversal/collect
    CPU_RECORD,       // CRender::Render — pass recording (ExecutePasses)
    CPU_END,          // CRender::End — submit + present (can block on queue/vsync)
    CPU_PHASE_COUNT
};
void CpuPhaseBegin(u32 idx);
void CpuPhaseEnd(u32 idx);
struct CpuPhaseScope {
    u32 i;
    explicit CpuPhaseScope(u32 idx) : i(idx) { CpuPhaseBegin(idx); }
    ~CpuPhaseScope() { CpuPhaseEnd(i); }
};

// ---------------------------------------------------------------------------
// CPU probes — RAII wall-time meters for ARBITRARY main-thread code (no
// VkCommandBuffer needed, unlike ZoneBegin — so they cover the helpers the pass
// zones can't see: streamer ticks, caster-queue rebuilds, tree/near-set walks,
// grass update...). Accumulate per frame (multiple scopes with the same slot
// sum up), reset in FrameEnd, printed as the 5-second [VK CPUprobes] line
// (r_profiler; vk_perf MARK includes it). Main thread only.
// ---------------------------------------------------------------------------
int  CpuProbeSlot(const char* name);        // intern a stable slot by name
void CpuProbeAdd(int slot, float ms);       // manual add (RAII scope preferred)
struct CpuProbeScope {
    int slot; u64 t0;
    explicit CpuProbeScope(int s);
    ~CpuProbeScope();
};
// One-liner: static slot + scope. Usage: VK_CPU_PROBE("cpu:TexStream");
#define VK_CPU_PROBE2(name, line) \
    static const int _cpslot##line = VK::Prof::CpuProbeSlot(name); \
    VK::Prof::CpuProbeScope _cpscope##line(_cpslot##line); \
    LoadProf::Scope _cplp##line(name) /* [rt prof]: a probe also names a part of a slow render-thread job */
#define VK_CPU_PROBE1(name, line) VK_CPU_PROBE2(name, line)
#define VK_CPU_PROBE(name) VK_CPU_PROBE1(name, __LINE__)

void Shutdown();

// ---------------------------------------------------------------------------
// Counters — incremented from the backend draw paths (best-effort; see .cpp).
// ---------------------------------------------------------------------------
void CountDraw(u32 instances, u32 tris);
void CountPipelineBind();
void CountDescriptorBind();

// ---------------------------------------------------------------------------
// Read-only getters for the overlay (Phase 2) and any diagnostics.
// ---------------------------------------------------------------------------
struct ZoneStat
{
    char  name[32];
    float gpuLast, gpuAvg, gpuMin, gpuMax;   // ms
    float cpuLast;                           // ms
    float ring[kHistory];                    // gpu ms history (oldest..newest via ringHead)
    u32   ringHead;
    u32   depth;                             // nesting level: 0 = top-level pass; children DON'T sum into gpu_total
};
struct MemSnap   { u64 usedBytes; u64 budgetBytes; u32 allocCount; u32 blockCount; };
struct FrameInfo { float cpuMs; float gpuMs; float fps; u32 draws, instances, tris, pipeBinds, descBinds; };

u32              GetZones(const ZoneStat** outArray);   // returns zone count
MemSnap          GetMem();
FrameInfo        GetFrame();

}} // namespace VK::Prof
