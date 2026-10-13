// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Texture streamer / VRAM residency manager.
//
// Solves the three problems the DX renderers solve and we didn't:
//   1. Honest DeferredLoad — the level load is bracketed and accounted, not stubbed.
//   2. texture_lod quality slider — top-mip skip on world/model diffuse.
//   3. Residency — a live VRAM budget (VK_EXT_memory_budget) that (a) caps mip levels
//      at LOAD so a heavy level fits instead of overcommitting (the Pripyat 7363/6313
//      hang), and (b) when r_txstream != 0, dynamically promotes/demotes mips per
//      frame UE-style (screen/LRU heuristic + transfer-queue reload + descriptor
//      rebind). The dynamic path is opt-in; the load-time cap + memory_priority are
//      always on and can only make things safer than the old load-everything path.
//
// Every CVulkanTexture built from a .dds registers here with its full-chain metadata;
// the object identity is stable across a streaming swap (CVulkanTexture::SwapContents)
// so WorldMaterial::tex pointers never dangle — only the descriptor's VkImageView
// changes, which we rebind through a callback the material cache installs.

#pragma once
#include "vk_core.h"
#include "vk_texture.h"

#include <unordered_map>
#include <functional>
#include <atomic>
#include <vector>
#include <utility>
#include <deque>
#include <thread>
#include <condition_variable>

namespace VK {

// Per-texture streaming record. `tex` owns the GPU image; this is bookkeeping.
struct StreamTexture
{
    CVulkanTexture* tex          = nullptr;
    shared_str      file;                 // resolved .dds (for re-reading mip ranges)
    u32             fullW        = 0;      // mip-0 width on disk
    u32             fullH        = 0;      // mip-0 height on disk
    u32             fullMips     = 0;      // total mips on disk
    VkFormat        format       = VK_FORMAT_UNDEFINED;
    u32             residentBase = 0;      // first mip currently resident (0 = full res)
    u32             wantedBase   = 0;      // heuristic target base mip
    VkDeviceSize    residentBytes= 0;      // current VRAM footprint (from residentBase)
    u32             lastUsedFrame= 0;      // Device.dwFrame of last Touch()
    TexStreamClass  klass        = TexStreamClass::UI;
    bool            streamable   = false;  // eligible for dynamic promote/demote

    // --- GPU feedback (UE-VT style, see RecordFeedbackResolve) --------------
    u32             fbSlot       = 0xFFFFFFFFu; // compact slot in the feedback SSBO
    u32             fbWantedBase = 0xFFFFFFFFu; // last decoded ABSOLUTE wanted base mip
    u32             fbSeenFrame  = 0;           // Device.dwFrame the GPU last sampled it
    u32             lastSwapFrame= 0;           // residency change frame (skip stale feedback)
    u8              demoteTicks  = 0;           // consecutive stream ticks feedback wanted coarser
    bool            ioPending    = false;       // async .dds read in flight — don't re-plan

    // Textures that live on the SAME surface as this one and therefore want the same
    // texel density, but get no feedback of their own: the material's normal+gloss
    // map. Only the base diffuse carries a feedback slot (one streamID fits in the
    // push block), so a companion's wanted mip is DERIVED from its base each tick —
    // see the propagation pass in StreamStep. Companions are shared by name across
    // materials, so one may be listed by several bases; the sharpest wins.
    std::vector<CVulkanTexture*> companions;
};

class TextureStreamer
{
public:
    static TextureStreamer& Instance();

    // --- Registration (called by CVulkanTexture) ----------------------------
    // `residentBase` is the mip actually uploaded (0 unless the load-time cap /
    // quality slider skipped some). `residentBytes` is that residency's footprint.
    void Register(CVulkanTexture* tex, const char* file, u32 fullW, u32 fullH,
                  u32 fullMips, VkFormat fmt, u32 residentBase,
                  VkDeviceSize residentBytes, TexStreamClass klass);
    void Unregister(CVulkanTexture* tex);

    // Tie a normal+gloss map to the base diffuse it shares a surface with, making it
    // eligible for dynamic streaming: it has no feedback slot, so its wanted mip is
    // derived from the base's every tick (matched by RESOLUTION, not mip index — the
    // two textures are often different sizes). Without this the whole Bump class can
    // only be capped at LOAD and never recovers, which on a tight budget means
    // permanently mushy normals. Idempotent; opting the base out cascades here.
    void LinkCompanion(CVulkanTexture* base, CVulkanTexture* companion);

    // Opt a texture out of (or back into) dynamic streaming after registration.
    // Used for terrain-material bases: their 15-binding splat set is captured by
    // side systems (vk_terrain_cache bake) that would keep a retired set handle.
    void SetStreamable(CVulkanTexture* tex, bool enable);

    // One-shot residency order: if the texture currently sits below `minDim`,
    // rebuild it at that floor NOW (synchronous transfer wait). For consumers that
    // bake the raw view and then opt out of streaming (the instanced tree/prop
    // path): whatever the load-time cap decided is frozen afterwards, so it must
    // be presentable first. Call BEFORE SetStreamable(tex, false).
    void EnsureMinResidency(CVulkanTexture* tex, u32 minDim);

    // Same order for a whole set, with ONE transfer wait for all of them (the
    // per-texture form waits once each — 65 tree materials cost 73 ms of load).
    void EnsureMinResidencyBatch(CVulkanTexture* const* texs, size_t count, u32 minDim);

    // --- Load-time residency decision ---------------------------------------
    // How many top mips to drop when first creating a texture of these dimensions.
    // Combines the manual `texture_lod` lever (WorldDiffuse only) with an automatic
    // budget-fit demotion that keeps live VRAM under (budget - headroom). Returns a
    // skip count clamped so the base never falls below kMinResidentDim.
    u32 PlanLoadMipSkip(u32 fullW, u32 fullH, u32 fullMips, VkFormat fmt,
                        TexStreamClass klass) const;
    // The decision itself; PlanLoadMipSkip wraps it to book the bytes.
    u32 PlanSkipUnaccounted(u32 fullW, u32 fullH, u32 fullMips, VkFormat fmt,
                            TexStreamClass klass) const;

    // Bytes this level will spend on things that are NOT textures -- geometry above
    // all -- which the live VRAM figure cannot see yet because they are allocated
    // after the textures are. Set by the loader from the size of level.geom.
    // Without it the load-time budget fit plans against a nearly empty card: with
    // r_tex_materialize the textures are built by workers that run AHEAD of the
    // geometry, and the measured result was 0 demotions and 3144 MB of textures on
    // a 6256 MB budget, which the streamer then evicted 16 a tick once play started.
    void SetLoadNonTexReserve(VkDeviceSize bytes) { m_LoadNonTexReserve = bytes; }

    // --- Level load bracketing (DeferredLoad / ResourcesDeferredUpload) ------
    void BeginLevelLoad();
    void EndLevelLoad();

    // Post-load residency trim: after a level finishes loading (device idle-able,
    // no frames in flight), if free device VRAM < r_txstream_reserve, demote the
    // largest world textures until the reserve is met. This is what actually caps a
    // heavy map (Pripyat) to fit alongside Streamline Frame-Gen — the load-time
    // per-texture cap can't, because textures load before the geometry/RT/FG bulk.
    // No-op on levels that already leave the reserve free (full quality kept).
    void EnforceLoadReserve();

    // --- Per-frame ----------------------------------------------------------
    void Frame();                       // drives dynamic streaming when r_txstream!=0
    void Touch(CVulkanTexture* tex);    // mark used this frame (from material bind)

    // Emergency REAL-VRAM release (mid-game DLSS enable): NGX allocates outside
    // VMA, so it needs the DRIVER to report ~mbNeeded free — our cached-block
    // holes don't count. Plans hard demotes to the 256px floor (invisible
    // textures first, then the largest visible) and routes them through the
    // async IO path; the retire ring then returns the old images (dedicated
    // allocations for big world diffuse) to the OS within a few frames.
    void FreeDeviceVram(u32 mbNeeded);

    // --- GPU feedback (UE-VT style "sampler feedback lite") ------------------
    // A device-local u32[kFeedbackSlots] SSBO (set 1 binding 30 of the EnvLight
    // set). The world fragment shaders atomicMin() the encoded desired LOD of the
    // base diffuse they actually sampled (1 of 16 pixels reports — enough for a
    // per-texture min). Each frame the buffer is copied into a host ring slot and
    // refilled with 0xFFFFFFFF ("not sampled"); the CPU reads the slot written
    // FRAMES_IN_FLIGHT frames ago (fence-proven complete) and turns it into
    // per-texture wanted mips — residency from FACT of visibility, not LRU.
    static constexpr u32 kFeedbackSlots = 8192;
    // Encoded LOD: enc = clamp((lodRelativeToResidentBase + 16) * 4, 0, 4095).
    // The +16 bias keeps magnification (negative lod = "want sharper") positive.

    // Feedback slot of a streamable texture (0xFFFFFFFF = none — push as-is to the
    // shader, it skips out-of-range IDs). Stable for the texture's lifetime.
    u32 GetFeedbackSlot(CVulkanTexture* tex) const;

    // The feedback SSBO (creates GPU buffers on first call; VK_NULL_HANDLE if
    // creation failed — callers then bind their dummy and no slots are handed out).
    VkBuffer GetFeedbackBuffer();

    // Record the per-frame resolve into the frame command buffer (right after it
    // opens, before any scene pass): copy last frame's feedback to the host ring
    // slot `frameSlot`, then re-fill the device buffer with 0xFFFFFFFF.
    void RecordFeedbackResolve(VkCommandBuffer cmd, u32 frameSlot);

    // Free GPU buffers + drain the retire ring. Call at device teardown (idle).
    void Shutdown();

    // --- Descriptor rebind hook (installed by WorldMaterialCache) -----------
    // Called after a streaming swap changes a texture's VkImageView so any descriptor
    // set that samples it gets rewritten. May be null (then those textures aren't
    // eligible for dynamic streaming).
    using RebindFn = std::function<void(CVulkanTexture*)>;
    void SetRebindCallback(RebindFn fn) { m_Rebind = std::move(fn); }

    // --- Reporting ----------------------------------------------------------
    void GetMemoryUsage(u32& outBytes, u32& outCount) const;
    void DumpStats() const;

    // Static: byte size of mips [base..fullMips) for a format at these mip-0 dims.
    static VkDeviceSize MipChainBytes(u32 w, u32 h, u32 fullMips, u32 base, VkFormat fmt);

private:
    TextureStreamer() = default;

    // One EnforceLoadReserve trim round against the live budget; returns true if
    // another round makes sense (reserve still unmet and something was demoted).
    bool EnforceLoadReserveRound(int round);

    // Texture VRAM budget (bytes) we try to stay under. From r_txstream_budget (MB)
    // if set, else derived from the live device-local budget minus headroom.
    VkDeviceSize BudgetBytes() const;

    // Dynamic streaming step (only when enabled): compute wanted mips from GPU
    // feedback (fallback: LRU when no feedback data is arriving, e.g. stale .spv),
    // then demote/promote, at most kMaxSwapsPerTick images.
    void StreamStep();
    void StreamStepLRU();   // pre-feedback heuristic, kept as the fallback

    // Execute a batch of (texture -> new base mip) residency changes: build the new
    // images on the transfer queue, wait the TRANSFER timeline (graphics never
    // stalls), swap handles + rebind descriptors, then RETIRE the old images —
    // freed by TickRetire once no in-flight frame can still sample them.
    // Shared by StreamStep and EnforceLoadReserve.
    void ApplyResidencyPlan(std::vector<std::pair<StreamTexture*, u32>>& plan);

    // Feedback internals (all called with s_Mutex NOT held unless noted).
    bool EnsureFeedbackGpu();           // create device+readback buffers once
    void ReadFeedback(u32 frameSlot);   // decode host ring slot into m_Tex records
    void TickRetire(bool force = false);// free retired images past the fence horizon

    // --- Async DDS IO worker -------------------------------------------------
    // The per-frame path must never touch the disk. StreamStep used to execute its
    // plan inline — up to 24 synchronous DDS reads + a transfer-timeline wait per
    // tick ON THE RENDER THREAD (the fast-flight stutter). Now the plan is queued
    // to a worker that reads whole .dds files into memory; DrainIoCompletions()
    // (every frame) creates the images, stages the copies on the async transfer
    // queue and swaps/rebinds — with NO CPU wait: the frame's graphics submit
    // already waits the upload timeline before FRAGMENT_SHADER (vk_command_buffer.h).
    struct IoJob  { CVulkanTexture* tex; shared_str file; u32 target; };
    // prebuilt: the image the IO thread already created from blob (texprebuild), still to be uploaded; else null.
    struct IoDone { CVulkanTexture* tex; shared_str file; u32 target; void* blob; size_t size; CVulkanTexture* prebuilt = nullptr; };
    void EnsureIoThread();
    void IoThreadMain();
    void EnqueuePlanIo(const std::vector<std::pair<StreamTexture*, u32>>& plan);
    void DrainIoCompletions();
    void StopIoThread();

    std::thread             m_IoThread;
    std::mutex              m_IoMutex;   // guards m_IoQueue, m_IoDone, m_IoStop
    std::condition_variable m_IoCv;
    std::deque<IoJob>       m_IoQueue;
    std::deque<IoDone>      m_IoDone;
    bool                    m_IoStop = false;

    // Per-frame application caps — bound the staging-ring memcpy + copy records a
    // single frame pays (the 64 MB ring wraps with a sync drain when overrun).
    static constexpr u32          kMaxApplyPerFrame    = 6;
    static constexpr VkDeviceSize kMaxApplyBytesFrame  = 16ull << 20;

    std::unordered_map<CVulkanTexture*, StreamTexture> m_Tex;
    VkDeviceSize m_TrackedBytes = 0;    // sum of residentBytes over m_Tex
    bool         m_InLevelLoad  = false;
    VkDeviceSize m_LoadNonTexReserve = 0;   // see SetLoadNonTexReserve
    VkDeviceSize m_LoadBaseUsage     = 0;   // credited VRAM sampled at BeginLevelLoad
    // Everything PlanLoadMipSkip has committed this load. Sixteen workers write it,
    // and the plan is a const method, so: mutable atomic.
    mutable std::atomic<VkDeviceSize> m_PlannedTexBytes{0};
    u32          m_LoadCapCount = 0;    // textures the load-time cap demoted this level
    u32          m_LastStreamFrame = 0;

    RebindFn     m_Rebind;

    // --- GPU feedback state --------------------------------------------------
    VkBuffer      m_FbDevice      = VK_NULL_HANDLE;  // u32[kFeedbackSlots], DEVICE_LOCAL
    VmaAllocation m_FbDeviceAlloc = VK_NULL_HANDLE;
    VkBuffer      m_FbRead        = VK_NULL_HANDLE;  // host-cached ring, x FRAMES_IN_FLIGHT
    VmaAllocation m_FbReadAlloc   = VK_NULL_HANDLE;
    const u32*    m_FbReadPtr     = nullptr;         // persistent map of m_FbRead
    bool          m_FbTried       = false;           // creation attempted (once)
    u32           m_FbAliveFrame  = 0;               // last frame ANY slot carried data
    u32           m_NextSlot      = 0;               // bump allocator for fbSlot
    std::vector<u32>             m_FreeSlots;        // recycled fbSlots
    std::vector<CVulkanTexture*> m_SlotOwner;        // fbSlot -> texture (decode map)

    // --- Retired images (swapped-out residencies awaiting the fence horizon) --
    struct RetiredImage { u32 frame; CVulkanTexture* holder; };
    std::vector<RetiredImage> m_Retired;

    // UE-style oversubscription bias: extra mips shaved off every VISIBLE wanted
    // level. Driven by promote STARVATION (quality promotes that found no room),
    // decayed on calm ticks; capped at +3 (the 256px visible floor dominates past
    // that). Whole part applies; slewed 0.25/tick (~1 mip/second at 60 fps).
    float m_OverBias = 0.0f;

    // Never demote a streamable texture below a 64px base — past that it's noise.
    static constexpr u32 kMinResidentDim   = 64;
    // Perceptual floor for ON-SCREEN textures: never demoted below 256px, and
    // rescued back to it unconditionally (budget ignored — a 256px BC chain is
    // ~90 KB; sub-256px surfaces in view read as broken, not as streaming).
    static constexpr u32 kVisibleFloorDim  = 256;
    // Throttle: at most this many image rebuilds per streaming tick (bounds the
    // per-tick transfer wait; swaps no longer device-idle so this can be generous).
    // Separate caps — a large invisible-demote backlog must never starve promotes.
    static constexpr u32 kMaxSwapsPerTick    = 8;    // LRU-fallback path
    static constexpr u32 kMaxPromotesPerTick = 8;    // full-set recovery in ~20-30 s
    static constexpr u32 kMaxDemotesPerTick  = 16;
    static constexpr u32 kMaxRescuesPerTick  = 24;   // rescue images are ~90 KB each
};

// Split of the LAST ApplyResidencyPlan, for the load-time callers that print it:
// rebuilding the images (DDS re-read + create + staged copy, serial) vs the one
// transfer wait vs the handle swap. See vk_texture_stream.cpp.
extern float g_lastResidencyBuildMs, g_lastResidencyFlushMs, g_lastResidencySwapMs;
extern u32   g_lastResidencyBuilt;
// Inside the rebuild, from the per-texture load profiler: is it the .dds read or
// the image create/upload? Different answers, different fixes.
extern float g_lastResidencyOpenMs, g_lastResidencyRepackMs,
             g_lastResidencyCreateMs, g_lastResidencyUploadMs, g_lastResidencyCloseMs;
extern float g_lastResidencyReadMs;   // the parallel pre-read, when r_tex_residency_threads > 0

} // namespace VK
