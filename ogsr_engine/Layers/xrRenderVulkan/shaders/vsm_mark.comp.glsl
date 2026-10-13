#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
// xrRenderVulkan — VSM page marking. One thread per scene pixel: reconstruct the
// world position from the prepass depth, map it into the sun clipmap, and mark the
// page it would sample as NEEDED. A unique-page counter (atomicOr-first-wins) gives
// a cheap "how many pages are visible this frame" for diagnostics. See vk_vsm.cpp.
#include "vsm_common.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D uDepth;   // scene prepass depth (statics+AT)

#define VSM_PARAMS_SET     0
#define VSM_PARAMS_BINDING 1
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)

layout(set = 0, binding = 2) buffer Needed  { uint needed[]; };   // per-level page flags
layout(set = 0, binding = 3) buffer Counter { uint uniquePages; }; // first-mark counter
layout(set = 0, binding = 4) buffer RMask   { uint rmask[]; };    // receiver mask: 2 u32/page, 8×8 sampled cells (r_vsm_rmask)
layout(set = 0, binding = 5) buffer Hits    { uint pageHits[]; }; // per-page sampled-pixel count (r_vsm_gaze refresh priority)

layout(push_constant) uniform Push {
    mat4 invViewProj;   // clip -> world (inverse of the scene view*proj)
    vec4 screen;        // xy = pixel dims, zw = 1/dims
    uint markStep;      // 1 = full-res, 2 = half-res (4x fewer threads + atomicOr)
    uint lodBias;       // throttle (r_vsm_throttle): mark N levels COARSER than the finest
                        // containing one — each +1 quarters the marked-page count (the only
                        // lever that reduces raster FILL, not just vertices). Receivers need
                        // no bias: they walk to the finest MAPPED level (vsm_resolve fallback).
    uint rmaskOn;       // r_vsm_rmask: also mark WHICH 8×8 cell of the page this pixel samples
    uint gazeOn;        // r_vsm_gaze: also count samples per page (pageHits) — the residency
                        // pass turns the counts into a refresh cadence ∝ on-screen footprint
} pc;

void main()
{
    // Half-res (markStep=2): one thread per 2x2 block samples one pixel. Adjacent pixels almost
    // always map to the SAME clipmap page (128 virtual texels) so 1-of-4 sampling rarely misses a
    // page; the page-boundary cases are covered by neighbouring blocks.
    ivec2 px = ivec2(gl_GlobalInvocationID.xy) * int(pc.markStep);
    if (px.x >= int(pc.screen.x) || px.y >= int(pc.screen.y)) return;

    vec2  uv   = (vec2(px) + 0.5) * pc.screen.zw;
    float zndc = texture(uDepth, uv).r;
    if (zndc >= 0.99999) return;   // sky / cleared — no receiver here

    // Reconstruct world position (D3D ndc, y-up — matches ssao.frag).
    vec4 clip  = vec4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, zndc, 1.0);
    vec4 world = pc.invViewProj * clip;
    world.xyz /= world.w;

    // World -> sun light space, then pick the finest clipmap level/page.
    vec3 lp = (vsm.view * vec4(world.xyz, 1.0)).xyz;
    vec2 luv; ivec2 page;
    int  L = vsmSelect(lp.xy, vsm.level, luv, page);
    if (L < 0) return;

    // Throttle LOD bias: promote to a coarser level. A point inside level L is inside
    // every coarser window too (camera-centred nested clipmap), so the promotion is a
    // straight re-projection; clamp guards the page-snapped window edges.
    int Lb = min(L + int(pc.lodBias), VSM_LEVELS - 1);
    if (Lb != L) {
        luv  = (lp.xy - vsm.level[Lb].xy) / vsm.level[Lb].z;
        page = clamp(ivec2(floor(luv * float(VSM_PAGES_AXIS))), ivec2(0), ivec2(VSM_PAGES_AXIS - 1));
        L = Lb;
    }

    uint idx  = uint(vsmPageIndex(L, page));

    // Subgroup dedup: neighbouring pixels almost always land in the SAME page
    // (128 virtual texels), so every lane hitting atomicOr on one word serialized
    // the whole subgroup at the atomic unit — the mark's real cost. One elected
    // lane covers the majority cluster; the rare minority lanes (page boundary
    // crossing the block) still atomic on their own. Zero quality change.
    uint first = subgroupBroadcastFirst(idx);
    if (idx == first) {
        if (subgroupElect()) {
            uint prev = atomicOr(needed[idx], 1u);
            if (prev == 0u) atomicAdd(uniquePages, 1u);
        }
    } else {
        uint prev = atomicOr(needed[idx], 1u);
        if (prev == 0u) atomicAdd(uniquePages, 1u);
    }

    // GAZE hits (r_vsm_gaze): per-page sampled-pixel count = the page's on-screen
    // footprint. Same subgroup aggregation as the mark: the elected lane adds the
    // whole majority-cluster size in ONE atomic, minority lanes pay their own.
    if (pc.gazeOn != 0u) {
        if (idx == first) {
            uvec4 same = subgroupBallot(true);   // lanes in the majority cluster (idx == first)
            if (subgroupElect()) atomicAdd(pageHits[idx], subgroupBallotBitCount(same));
        } else {
            atomicAdd(pageHits[idx], 1u);
        }
    }

    // RECEIVER MASK (r_vsm_rmask): also record WHICH 8×8 cell (16 virtual texels) this
    // pixel samples. Same subgroup-dedup idea, keyed on (page, cell): a cell is 16
    // texels so neighbouring pixels still mostly share a key — one elected atomic
    // covers the majority cluster, minority lanes pay their own.
    if (pc.rmaskOn != 0u) {
        vec2  pl   = luv * float(VSM_PAGES_AXIS) - vec2(page);   // [0,1) within the page
        ivec2 cell = clamp(ivec2(floor(pl * 8.0)), ivec2(0), ivec2(7));
        uint  bitI = uint(cell.y) * 8u + uint(cell.x);
        uint  word = idx * 2u + (bitI >> 5u);
        uint  bit  = 1u << (bitI & 31u);
        uint  key  = idx * 64u + bitI;
        uint  kf   = subgroupBroadcastFirst(key);
        if (key == kf) { if (subgroupElect()) atomicOr(rmask[word], bit); }
        else atomicOr(rmask[word], bit);
    }
}
