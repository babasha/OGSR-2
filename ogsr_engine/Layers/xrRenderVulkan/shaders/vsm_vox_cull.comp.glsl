#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM voxel-BRICK per-page cull (STAGE 2 of the brick caster,
// r_vsm_tree_hull_vox_cull). UE-parity piece: UE rasterizes into a shadow page only
// the bricks that overlap it; our stage-1 path instanced EVERY brick of a tree over
// EVERY page of its shadow column (~50-100 (page × full-slice) VS sweeps per tree —
// the dominant vertex cost of the brick caster, and the reason the static tier had
// to fall back to the merged shell).
//
// One workgroup per tree, run twice (push.mode 0 = dyn atlas, 1 = static). Reads the
// per-tree VkDrawIndirectCommand vsm_hull_cmd just wrote (bcount already carries the
// band prefix-cut), and for each of the tree's pages:
//   count → shared-memory scan → one atomicAdd carves a contiguous BrickList range →
//   re-test writes the surviving GLOBAL brick indices → ONE compacted command
//   {6·survivors, 1, firstVertex = 6·listBase, firstInstance = treeCap slot} per
//   (tree,page). The VS keeps its exact page routing (gl_InstanceIndex unchanged) and
//   fetches bricks through the list (pc flag) — original indices survive, so the
//   band's position-ordered dissolve is untouched.
// Test = brick bounding circle (half-diagonal × instance scale + wind slop) vs the
// page rect in light XY — same conservative shape as the crown meshlet stage 2.
//
// + SHADOW-HZB OCCLUSION (pc.hzbOn, r_vsm_hzb — the UE VirtualShadowMap two-pass idea):
// a brick whose NEAREST light-depth is farther than the prior-frame occluder max over
// the 16×16-texel blocks its footprint covers (vsm_hzb_reduce.comp) is fully behind
// the cached canopy/walls/terrain in that page → can't win the LESS_OR_EQUAL depth
// test → culled here. In a dense forest only the sun-facing crown layer writes depth;
// this kills the whole understory's VS+raster. mode 1 (static): occluder = the page's
// own cached slot. mode 0 (dyn): the dyn atlas has no cache — occluder = the STATIC
// slot of the same world page via staticPT (Option A, same as vsm_tree_bin).
// Conservative: block max + margin, nearest point of the brick sphere.
#include "vsm_common.glsl"

layout(local_size_x = 64) in;

struct TreeInst { mat4 xform; float c_scale_hemi; float c_bias_hemi; uint _p0; uint _p1; };
struct GpuBrick { vec3 p; uint meta; uvec4 masks; };

layout(set = 0, binding =  0) readonly buffer VoxCmdD    { uint vd[]; };     // dyn brick cmds (4 u32/tree, from vsm_hull_cmd)
layout(set = 0, binding =  1) readonly buffer VoxCmdS    { uint vst[]; };    // static brick cmds
layout(set = 0, binding =  2) readonly buffer Choice     { uvec4 choice[]; };// z = floatBits(cellEdge mesh-local)
layout(set = 0, binding =  3) readonly buffer Bricks     { GpuBrick brick[]; };
layout(set = 0, binding =  4) readonly buffer Xform      { TreeInst inst[]; };
layout(set = 0, binding =  5) readonly buffer CasterPages{ uint casterPages[]; };
layout(set = 0, binding =  6) readonly buffer PageListD  { uvec4 pageListD[]; };  // dyn slot -> (L, page.xy)
layout(set = 0, binding =  7) readonly buffer PageListS  { uvec4 pageListS[]; };  // static slot -> (L, page.xy)
#define VSM_PARAMS_SET     0
#define VSM_PARAMS_BINDING 8
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)
layout(set = 0, binding =  9) writeonly buffer OutCmdD   { uint od[]; };     // compacted VkDrawIndirectCommand[]
layout(set = 0, binding = 10) writeonly buffer OutCmdS   { uint ost[]; };
layout(set = 0, binding = 11) writeonly buffer BrickList { uint blist[]; };  // surviving global brick indices
layout(set = 0, binding = 12) buffer Stats { uint stats[]; };                // [0]=cmdCountD [1]=cmdCountS [2]=listCount [3]=overflow [4]=hzbCulled
layout(set = 0, binding = 13) readonly buffer PageMaxBlk { float pageMaxBlk[]; };  // shadow-HZB: 64 block maxes per STATIC slot
layout(set = 0, binding = 14) readonly buffer StaticPT   { uint staticPT[]; };     // virtual page -> STATIC slot (mode 0 occluder lookup)
layout(set = 0, binding = 15) readonly buffer RMask      { uint rmask[]; };        // receiver mask: 2 u32/page, 8×8 sampled cells (r_vsm_rmask, mode 0 only)
layout(set = 0, binding = 16) readonly buffer TreeList   { uint treeList[]; };     // compact dispatch: indices of trees with a live brick choice

// pc.count = LENGTH OF treeList (one workgroup per LISTED tree) — the CPU builds the
// list from this frame's brick choices, so the ~2600 empty per-tree workgroups the
// old full-range dispatch burned (×2 modes) never launch.
layout(push_constant) uniform PC { uint count; uint mode; uint cmdCap; uint listCap; float slop; uint hzbOn; float margin; uint rmaskOn; } pc;

shared uint shCnt[64];
shared uint shBase;

// Per-(brick,page) test. 0 = outside the page rect, 1 = survives, 2 = XY-hit but fully
// behind the cached occluders (shadow-HZB), 3 = XY-hit but no visible receiver samples
// the cells under its footprint (receiver mask). sBase = sslot*64 into pageMaxBlk, or
// 0xFFFFFFFF when occlusion is off/unmapped. rmW = the page's receiver-mask words
// (uvec2(~0) = mask off/full). pmin0 = RAW page min (no R expansion). The mask cell
// grid IS the HZB block grid (8×8 of 16 texels) — one footprint rect serves both.
uint brickTest(uint gbi, mat4 VX, vec3 cOfs, float R, vec2 pmin, vec2 pmax,
               uint sBase, vec2 pmin0, float blkW, uvec2 rmW)
{
    vec3 lc = (VX * vec4(brick[gbi].p + cOfs, 1.0)).xyz;
    if (any(lessThan(lc.xy, pmin)) || any(greaterThan(lc.xy, pmax))) return 0u;
    const bool wantMask = (rmW.x & rmW.y) != 0xFFFFFFFFu;
    if (sBase != 0xFFFFFFFFu || wantMask) {
        // Blocks/cells the brick's bounding circle covers (clamped to this page's 8×8 grid).
        ivec2 b0 = clamp(ivec2(floor((lc.xy - vec2(R) - pmin0) / blkW)), ivec2(0), ivec2(7));
        ivec2 b1 = clamp(ivec2(floor((lc.xy + vec2(R) - pmin0) / blkW)), ivec2(0), ivec2(7));
        // Receiver mask (r_vsm_rmask): no sampled cell under the footprint → invisible depth.
        if (wantMask) {
            uvec2 rm = vsmRectMask8(b0, b1);
            if (((rmW.x & rm.x) | (rmW.y & rm.y)) == 0u) return 3u;
        }
        if (sBase != 0xFFFFFFFFu) {
            float occ = 0.0;
            for (int by = b0.y; by <= b1.y; ++by)
                for (int bx = b0.x; bx <= b1.x; ++bx)
                    occ = max(occ, pageMaxBlk[sBase + uint(by) * 8u + uint(bx)]);
            // Nearest possible normalized depth of the brick (sphere front point) vs the
            // farthest cached depth under its footprint: strictly behind → can't win LEQUAL.
            if ((lc.z - R - vsm.zparams.x) * vsm.zparams.y > occ + pc.margin) return 2u;
        }
    }
    return 1u;
}

void main()
{
    if (gl_WorkGroupID.x >= pc.count) return;
    uint t = treeList[gl_WorkGroupID.x];
    uint q = t * 4u;
    uint vcount, pages, first, fInst;
    if (pc.mode == 0u) { vcount = vd[q];  pages = vd[q+1u];  first = vd[q+2u]  / 6u; fInst = vd[q+3u];  }
    else               { vcount = vst[q]; pages = vst[q+1u]; first = vst[q+2u] / 6u; fInst = vst[q+3u]; }
    uint bcount = vcount / 6u;
    if (bcount == 0u || pages == 0u) return;

    float cell = uintBitsToFloat(choice[t].z);
    mat4  X    = inst[t].xform;
    float maxScale = max(length(X[0].xyz), max(length(X[1].xyz), length(X[2].xyz)));
    float R    = 2.0 * cell * 1.7320508 * maxScale + pc.slop;   // brick half-diagonal (half-edge 2·cell) + wind slop
    mat4  VX   = vsm.view * X;                                  // mesh-local → light space
    vec3  cOfs = vec3(2.0 * cell);                              // brick min corner → center

    uint lane = gl_LocalInvocationID.x;
    uint occCnt = 0u;   // shadow-HZB culled (this lane, all pages) → stats[4]
    uint rmCnt  = 0u;   // receiver-mask culled (this lane, all pages) → stats[5]
    for (uint pi = 0u; pi < pages; ++pi)
    {
        uint  slot = casterPages[fInst + pi] & 0x1FFFu;   // arena entry = (tree<<13)|slot
        uvec4 pg   = (pc.mode == 0u) ? pageListD[slot] : pageListS[slot];
        int   L    = int(pg.x);
        float pw   = vsm.level[L].z / float(VSM_PAGES_AXIS);
        vec2  pmin0 = vsm.level[L].xy + vec2(pg.yz) * pw;   // raw page rect (block addressing)
        vec2  pmin = pmin0 - vec2(R);
        vec2  pmax = pmin0 + vec2(pw) + vec2(R);
        float blkW = pw * 0.125;                            // 8 blocks per page axis

        // Occluder base for this page: static tier = its own cached slot; dyn tier =
        // the STATIC slot of the same world page (Option A). UNMAPPED → no occlusion.
        uint sBase = 0xFFFFFFFFu;
        if (pc.hzbOn != 0u) {
            uint sslot = (pc.mode == 1u) ? slot : staticPT[uint(vsmPageIndex(L, ivec2(pg.yz)))];
            if (sslot != VSM_UNMAPPED) sBase = sslot * 64u;
        }

        // Receiver mask (r_vsm_rmask): the page's sampled-cell words. DYN tier only
        // (mode 0) — masked static pages would cache incomplete. uvec2(~0) = off/full
        // (brickTest skips the test on that value).
        uvec2 rmW = uvec2(0xFFFFFFFFu);
        if (pc.rmaskOn != 0u && pc.mode == 0u) {
            uint vpi = uint(vsmPageIndex(L, ivec2(pg.yz)));
            rmW = uvec2(rmask[vpi * 2u], rmask[vpi * 2u + 1u]);
        }

        // Pass 1: count my survivors.
        uint mine = 0u;
        for (uint bi = lane; bi < bcount; bi += 64u) {
            uint r = brickTest(first + bi, VX, cOfs, R, pmin, pmax, sBase, pmin0, blkW, rmW);
            if (r == 1u) ++mine;
            else if (r == 2u) ++occCnt;
            else if (r == 3u) ++rmCnt;
        }
        shCnt[lane] = mine;
        barrier();
        // Inclusive Hillis-Steele scan over the 64 lanes (barrier-safe two-phase).
        for (uint off = 1u; off < 64u; off <<= 1u) {
            uint v   = shCnt[lane];
            uint add = (lane >= off) ? shCnt[lane - off] : 0u;
            barrier();
            shCnt[lane] = v + add;
            barrier();
        }
        uint total = shCnt[63];
        if (lane == 63u) {
            shBase = 0xFFFFFFFFu;
            if (total != 0u) {
                uint base = atomicAdd(stats[2], total);
                if (base + total <= pc.listCap) {
                    uint ci = atomicAdd(stats[pc.mode], 1u);
                    if (ci < pc.cmdCap) {
                        uint o = ci * 4u;
                        if (pc.mode == 0u) { od[o] = 6u * total; od[o+1u] = 1u; od[o+2u] = 6u * base; od[o+3u] = fInst + pi; }
                        else               { ost[o] = 6u * total; ost[o+1u] = 1u; ost[o+2u] = 6u * base; ost[o+3u] = fInst + pi; }
                        shBase = base;
                    } else atomicAdd(stats[3], 1u);
                } else atomicAdd(stats[3], 1u);
            }
        }
        barrier();
        // Pass 2: re-test and write my survivors at my scanned offset.
        if (shBase != 0xFFFFFFFFu && mine != 0u) {
            uint w = shBase + shCnt[lane] - mine;   // exclusive prefix
            for (uint bi = lane; bi < bcount; bi += 64u) {
                if (brickTest(first + bi, VX, cOfs, R, pmin, pmax, sBase, pmin0, blkW, rmW) == 1u)
                    blist[w++] = first + bi;
            }
        }
        barrier();
    }
    if (occCnt != 0u) atomicAdd(stats[4], occCnt);
    if (rmCnt  != 0u) atomicAdd(stats[5], rmCnt);
}
