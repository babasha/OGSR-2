// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  CLUSTER-LOD PAGE STREAMING (Stage B of the streaming-world architecture)
// ============================================================================
//  Nanite-style residency for the cluster-LOD index data: instead of keeping
//  the whole cluster IB resident (~340 MB on Pripyat-sized levels), the bake
//  packs clusters into ~128 KB PAGES (group-contiguous, spatially ordered);
//  at runtime only a budgeted pool of pages lives in VRAM:
//    - PINNED pages (DAG roots + the coarsest slice) are always resident, so
//      every subtree has a drawable ancestor — no holes, ever.
//    - The cull shader consults a 2-bit per-entry state (drawable / streaming
//      leaf): a non-resident cut is covered by its coarsest RESIDENT ancestor
//      drawing as a leaf (UE STREAMING_LEAF recipe), and the shader appends a
//      page REQUEST to a feedback buffer (same readback-ring pattern as the
//      texture streamer).
//    - The CPU decodes requests, streams pages from the cluster cache file on
//      an IO thread, installs them into pool slots and republishes the state
//      bits + per-page firstIndex bases. Draw shaders never change: the cull
//      writes firstIndex = pageSlotBase[page] + pageLocalIbFirst.
//  Group-atomicity under streaming: the CPU derives a memoized LIVENESS per
//  DAG group (all pages resident AND all parent entries live) — descendants
//  of a broken level are suppressed wholesale, so a leaf-drawn ancestor never
//  overlaps finer geometry.
//  r_clpage 0 (default until verified) = eager install-all: identical VRAM
//  and behavior to the pre-streaming renderer, same shader path.
// ============================================================================
#pragma once
#include "HW_Vulkan.h"
#include "vk_world_gpu.h"   // GpuMeshMeta

namespace VK { namespace ClusterStream {

constexpr u32 kPageBytes   = 128 * 1024;   // IB streaming granularity (Nanite-sized)
constexpr u32 kPageVbBytes = 384 * 1024;   // VB payload cap per page (slice 2)
constexpr u32 kVbStride    = 32;           // the one repackable vertex stride (lmap + vlit)

// One page of cluster data. byteOff/byteSize address the page's INDEX payload
// inside its index-type blob; vbByteOff/vbByteSize address its VERTEX payload
// inside the single vertex blob (slice 2 repack; 0/0 = raw page, entries keep
// absolute first_vertex into the ORIGINAL pool VB). The assembler emits all
// blobs page-ordered. Persisted in the cluster cache (v12) after the records.
struct PageRec {
    u32 flags;      // bit0 = u32 pool (else u16), bit1 = pinned, bit2 = has VB payload
    u32 byteOff;    // IB offset into its type's blob
    u32 byteSize;   // IB payload bytes (<= kPageBytes)
    u32 vbByteOff;  // offset into the vertex blob (kVbStride-aligned)
    u32 vbByteSize; // vertex payload bytes (<= kPageVbBytes)
};
constexpr u32 kPageIB32   = 1u << 0;
constexpr u32 kPagePinned = 1u << 1;
constexpr u32 kPageHasVB  = 1u << 2;

// ---- Bake time (BuildClusters, BEFORE SaveClusterCache) --------------------
// Per-MESH vertex source info: which host VB copy the mesh's entries index
// (global vertex id = meta.first_vertex + index for BOTH the u16 rel-vBase and
// the u32 pool-global paths), and whether its vertices repack into pages
// (stride == kVbStride; anything else stays on the original pool VB).
struct VbSource  { const u8* data; u64 bytes; u32 stride; };
struct MeshVbInfo { u32 srcId; u8 repack; };

// Packs cluster entries into pages: identity page 0 reserved, then pinned zone
// (parentError==INF roots + coarsest groups up to a byte quota), then streamed
// zone in (morton, coarseness) order. Reorders idx16/idx32 page-contiguously,
// rewrites meta ib_first to PAGE-LOCAL index offsets and stores page ids in
// meta._pad1. entryIb32[i] = 1 if entry i's indices live in the u32 blob.
// Slice 2: entries of repackable meshes get their UNIQUE vertices copied into
// outVbBlob per page (page-wide dedup), their indices rewritten PAGE-LOCAL and
// first_vertex set page-local (0-based); the cull adds the page's VB slot base.
// entryMesh[i] -> meshInfo index; pages never mix repacked and raw entries.
void AssemblePages(xr_vector<WorldGPU::GpuMeshMeta>& meta,
                   const xr_vector<u8>& entryIb32,
                   const xr_vector<u32>& entryMesh,
                   const xr_vector<MeshVbInfo>& meshInfo,
                   const xr_vector<VbSource>& sources,
                   xr_vector<u16>& idx16, xr_vector<u32>& idx32,
                   xr_vector<u8>& outVbBlob,
                   xr_vector<PageRec>& outPages);

// ---- Level lifetime (two phases, both from WorldGPU::Build) -----------------
// Phase 1 — BEFORE draw grouping (the group key needs the pool handles):
// creates the pools, installs pinned pages (or everything when r_clpage 0 /
// no file). `filePath` + blob offsets = the streaming source (cluster cache
// file); ramIdx16/32/ramVb = in-RAM source used on a fresh bake or as fallback
// when the file is unavailable (the latter forces eager install-all).
// Slice 2: streaming mode gives every IB slot a matching VB slot (kPageVbBytes)
// — a page's IB+VB halves install/evict atomically; eager mode keeps the whole
// vertex blob resident in one exact-size buffer.
bool CreatePools(const xr_vector<PageRec>& pages,
                 const char* filePath, u64 blobOff16, u64 blobOff32, u64 blobOffVb,
                 const xr_vector<u16>* ramIdx16, const xr_vector<u32>* ramIdx32,
                 const xr_vector<u8>* ramVb);
// Phase 2 — AFTER the final meta array is assembled (clusters + plain meshes;
// page ids in _pad1): group/liveness tables, state + feedback buffers, IO
// thread. Returns false on alloc failure — caller disables the WorldGPU path.
bool BuildState(const xr_vector<WorldGPU::GpuMeshMeta>& finalMeta);
void Destroy();
bool Active();

// GPU buffers (valid after Build) — bound by the cull / VSM-bin shaders.
VkBuffer PoolIB16();        // u16 cluster index pool (bind as index buffer)
VkBuffer PoolIB32();        // u32 cluster index pool
VkBuffer PoolVB();          // repacked cluster vertex pool (bind as vertex buffer)
VkBuffer BitsBuffer();      // 2 bits/entry packed 16-per-u32: bit0 drawable, bit1 leaf
VkBuffer SlotBaseBuffer();  // per-page PAIRS: [2p] firstIndex base (index units of
                            // its pool), [2p+1] firstVertex base (kVbStride units)
VkBuffer RequestBuffer();   // [0] = atomic count, then entryId | urgency<<24
VkBuffer TouchedBuffer();   // page bitset (atomicOr by drawn entries → LRU)

// Host mirror of the 2-bit state (the cpu-cut diag replica reads it so its
// counts keep matching the GPU cut under streaming). Null when inactive.
const u32* HostBits();
// Monotonic counter bumped on every page residency flip. Consumers that CACHE a
// snapshot of the resident world (the RT scene bakes one into a BLAS) compare it
// against the value they built with to notice that streaming moved underneath
// them - the alternative is a structure built on a cold cache and kept forever.
u32 ResidencyGen();

// Dump the entry's member/child group state incl. every missing page (why is
// this cohort broken / why does this entry draw as a streaming leaf).
void DebugEntryChain(u32 e);

// Per-frame: Frame() decodes the fence-proven feedback slot, drains IO
// completions, allocates/evicts slots and schedules reads (call from
// CRender::Begin BEFORE the frame cmd opens, next to TextureStreamer::Frame).
// RecordFrameOps records into the frame cmd (BEFORE any world cull): feedback
// readback+reset, staged page-data copies, dirty bits/slotBase uploads.
void Frame();
void RecordFrameOps(VkCommandBuffer cmd, u32 frameSlot);

}} // namespace VK::ClusterStream
