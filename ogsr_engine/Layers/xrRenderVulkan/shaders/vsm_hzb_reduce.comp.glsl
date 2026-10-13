#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — shadow-HZB reduce (r_vsm_hzb). One workgroup per STATIC-atlas physical slot;
// reduces that slot's PRIOR-FRAME depth into occluder maxes the caster bins/culls test against:
//   pageMax[slot]        — one MAX per 128² page (coarse; vsm_tree_bin whole-tree × page test)
//   pageMaxBlk[slot*64+i] — 8×8 grid of per-16×16-texel-block maxes (fine; vsm_vox_cull tests each
//                           BRICK's footprint against only the blocks it covers — a canopy gap no
//                           longer defeats occlusion for the whole page, only for casters under it)
// A caster whose NEAREST light-depth is farther than the max over its footprint is fully behind
// the cached occluders (canopy/walls/terrain) → can't win the LESS_OR_EQUAL depth test → skip its
// raster. CONSERVATIVE (MAX-reduce, strictly-behind only) → shadow depth identical.
//
// PERSISTENT + INCREMENTAL (v2): the outputs live across frames — a cached page's depth only
// changes when the page is re-rendered, so only slots that were DIRTY LAST FRAME (prevDirty,
// binding 1 = the prior frame's slotDirty copy) are re-reduced; slots whose cached tile got
// evicted/remapped (priorValid==0, from residency's physTile match) are reset to 1.0 = never
// occludes. Everything else early-outs after two buffer reads, so the reduce cost scales with
// the sun's dirty rate (~100 pages/frame), not with total residency. Host fills both outputs
// with 1.0 on init / InvalidateCache / r_vsm_hzb OFF→ON flip (stale-content hazard).
// Runs at MarkPages time = atlas still holds prior-frame depth (SHADER_READ, before RenderAtlas
// overwrites this frame's dirty pages). See vk_vsm.cpp DispatchHzbReduce.
#include "vsm_common.glsl"

layout(local_size_x = 16, local_size_y = 16) in;

layout(set = 0, binding = 0) uniform sampler2D atlas;                        // STATIC atlas depth (prior frame)
layout(set = 0, binding = 1) readonly buffer PrevDirty  { uint prevDirty[];  };  // LAST frame's slotDirty (content changed → re-reduce)
layout(set = 0, binding = 2) readonly buffer PriorValid { uint priorValid[]; };
layout(set = 0, binding = 3) buffer PageMax             { float pageMax[];   };
layout(set = 0, binding = 4) buffer PageMaxBlk          { float pageMaxBlk[]; };  // 64 per slot: [slot*64 + by*8 + bx]

shared float s_max[256];

void main()
{
    uint slot = gl_WorkGroupID.x;
    if (slot >= uint(VSM_MAX_PHYS_S)) return;
    uint li = gl_LocalInvocationID.y * 16u + gl_LocalInvocationID.x;

    // Evicted/remapped slot: the cached depth belongs to a DIFFERENT world tile — reset to
    // "never occludes" (256 threads → 64 blocks + total in one pass) and be done.
    if (priorValid[slot] == 0u) {
        if (li < 64u) pageMaxBlk[slot * 64u + li] = 1.0;
        if (li == 0u) pageMax[slot] = 1.0;
        return;
    }
    // Valid and unchanged since the last reduce → the persisted maxes are still exact.
    if (prevDirty[slot] == 0u) return;

    uint ax   = slot % uint(VSM_ATLAS_W_S);
    uint ay   = slot / uint(VSM_ATLAS_W_S);
    ivec2 org = ivec2(int(ax) * VSM_PAGE_SIZE, int(ay) * VSM_PAGE_SIZE);

    // 16x16 threads over a 128x128 page → each thread maxes an 8x8 texel tile.
    float m = 0.0;
    ivec2 t0 = org + ivec2(int(gl_LocalInvocationID.x) * 8, int(gl_LocalInvocationID.y) * 8);
    for (int dy = 0; dy < 8; ++dy)
        for (int dx = 0; dx < 8; ++dx)
            m = max(m, texelFetch(atlas, t0 + ivec2(dx, dy), 0).r);

    s_max[li] = m;
    barrier();

    // Block maxes: block (bx,by) = 16×16 texels = the 2×2 thread tiles at (2bx..2bx+1, 2by..2by+1).
    if (li < 64u) {
        uint bx = li & 7u, by = li >> 3u;
        uint t  = (by * 2u) * 16u + bx * 2u;
        pageMaxBlk[slot * 64u + li] = max(max(s_max[t], s_max[t + 1u]), max(s_max[t + 16u], s_max[t + 17u]));
    }
    barrier();   // block reads of s_max done before the destructive tree reduction below

    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (li < s) s_max[li] = max(s_max[li], s_max[li + s]);
        barrier();
    }
    if (li == 0u) pageMax[slot] = s_max[0];
}
