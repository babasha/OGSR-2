#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM grass-caster binning, near/far STATIC-CACHE HYBRID. One thread
// per grass instance slot in the GPU-driven detail buffer (VisibleSSBO, previous frame
// — grass gen runs AFTER the VSM atlas in the frame, so casting uses 1-frame-stale
// instances; invisible since grass is near-static). For each VALID, NEAR instance
// append a COMPACT (instance, page-slot) PAIR for every accepted page its light-space
// footprint overlaps. The page pass draws exactly pairCount[type] instances per type
// via indirect. TWO dispatches per frame over this shader, selected by push.mode
// (same split the tree hybrid uses, but PAGE-LEVEL based — no per-instance flags):
//   mode 0  DYNAMIC atlas, L0 only (±12 m): resident dyn pages, re-rendered every
//           frame WITH live wind — sway is only readable up close; marks dynUsed.
//   mode 1  STATIC atlas, L1..L2 (12..48 m): DIRTY static pages only (slotDirty =
//           the toroidal cache filter), rendered rigid — a cached far page keeps its
//           grass depth until sun motion / window scroll dirties it (≈ free standing).
//   mode 2  DYNAMIC atlas, L0..L2 — fallback when the static hybrid is disabled
//           (r_vsm_grass_static 0): everything every-frame like before.
// The receiver reads the FINEST resident level per point (vsmSelect), so the L0/L1
// boundary is exact and needs no transition logic: pages entering/leaving the L0
// window are re-marked/re-rendered by the existing residency machinery.
#include "vsm_common.glsl"

layout(local_size_x = 64) in;

struct DetailInstance { vec4 row0, row1, row2, color; };   // 64 B (matches DetailInstance; translation = row*.w)
layout(set = 0, binding = 0) readonly buffer Visible  { DetailInstance inst[]; };
layout(set = 0, binding = 1) readonly buffer Indirect { uint indirect[]; };       // 5 u32 / type, instanceCount @ +1
#define VSM_PARAMS_SET     0
#define VSM_PARAMS_BINDING 2
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)
layout(set = 0, binding = 3) readonly buffer PageTable { uint pageTable[]; };      // DYN table (mode 0/2) or STATIC table (mode 1)
layout(set = 0, binding = 4) writeonly buffer GrassPairs { uint pairs[]; };        // per-type arena: slot(13) << 19 | instLocal(19)
layout(set = 0, binding = 5) buffer Stats     { uint stats[]; };                   // [0]=casting instances [1]=dropped [2]=dyn pairs [3]=static pairs [4]=candidates
layout(set = 0, binding = 6) buffer DynUsed   { uint dynUsed[]; };                 // mode 0/2: dyn slot -> has-caster flag (resolve gate). mode 1: slotDirty (READ — dirty static pages)
layout(set = 0, binding = 7) buffer PairCount { uint pairCount[]; };               // per-type append counters (-> indirect instanceCount)

layout(push_constant) uniform Push {
    uint sectionSize;   // per-type stride in VisibleSSBO (instances)
    uint typeCount;
    uint pairSection;   // per-type pair-arena capacity (per-mode buffer)
    uint mode;          // 0 = dyn L0 | 1 = static L1..L2 dirty | 2 = dyn L0..L2 (no-hybrid fallback)
    vec4 camRange;      // xyz = camera world pos, w = max cast distance (near cull)
} pc;

void main()
{
    uint g = gl_GlobalInvocationID.x;
    if (g >= pc.sectionSize * pc.typeCount) return;

    uint type  = g / pc.sectionSize;
    uint local = g - type * pc.sectionSize;
    uint count = indirect[type * 5u + 1u];          // stale instanceCount for this type
    if (local >= count) return;

    DetailInstance di = inst[g];
    vec3 center = vec3(di.row0.w, di.row1.w, di.row2.w);
    if (distance(center, pc.camRange.xyz) > pc.camRange.w) return;
    if (pc.mode != 1u) atomicAdd(stats[4], 1u);   // diag: candidates past the count+distance culls (count once)

    // Append a pair for EVERY accepted page the tuft's light-space footprint
    // overlaps, per level up to a 4x4 window. An L0 page is only 0.75 m — smaller
    // than a tuft — and the page VS hard-clips to page bounds; a 4-CORNER probe
    // missed the MIDDLE pages whenever the footprint spanned 3 pages (low sun
    // stretches a blade's footprint along the light-space up axis), cutting shadows
    // into page-grid squares. Footprint = segment base -> base + lightUp*Hmax,
    // inflated laterally by R; the window clamp keeps the BASE page (contact shadow
    // wins if a very grazing sun stretches further).
    // R covers the WIDEST detail geometry, not just a blade: flat ground ferns put
    // fronds ~1 m from the instance centre — R=0.5 left their outer pages unbinned
    // and the page clip cut those shadows along straight 0.75 m page-grid edges
    // (user-visible). Same reason the window is 4x4, not 3x3: R=1 alone widens the
    // bb past 3 pages, and afternoon sun stretches a 1.4 m blade past 2.25 m too.
    const float R    = 1.0;    // lateral half-extent (m, light XY == world m: rotation)
    const float Hmax = 1.4;    // conservative tall-blade height (m)
    vec2  lp  = (vsm.view * vec4(center, 1.0)).xy;
    vec2  upL = (vsm.view * vec4(0.0, Hmax, 0.0, 0.0)).xy;   // tip offset in light XY
    vec2  bbMin = min(lp, lp + upL) - vec2(R);
    vec2  bbMax = max(lp, lp + upL) + vec2(R);
    bool any_ = false;
    const bool st = (pc.mode == 1u);
    const uint slotCap = st ? uint(VSM_MAX_PHYS_S) : uint(VSM_MAX_PHYS);
    int L0 = st ? 1 : 0;
    int L1 = (pc.mode == 0u) ? 1 : 3;
    for (int L = L0; L < L1; ++L) {
        vec2  o   = vsm.level[L].xy;
        float pw  = vsm.level[L].z / float(VSM_PAGES_AXIS);
        ivec2 pBase = ivec2(floor((lp    - o) / pw));
        ivec2 w0 = clamp(ivec2(floor((bbMin - o) / pw)), pBase - ivec2(3), pBase);
        ivec2 w1 = min(ivec2(floor((bbMax - o) / pw)), w0 + ivec2(3));
        for (int py = w0.y; py <= w1.y; ++py)
        for (int px = w0.x; px <= w1.x; ++px) {
            if (px < 0 || px >= VSM_PAGES_AXIS || py < 0 || py >= VSM_PAGES_AXIS) continue;
            uint slot = pageTable[vsmPageIndex(L, ivec2(px, py))];
            if (slot >= slotCap) continue;              // not resident (incl. UNMAPPED)
            if (st && dynUsed[slot] == 0u) continue;    // static cache: dirty pages only (binding 6 = slotDirty here)
            uint idx = atomicAdd(pairCount[type], 1u);
            if (idx >= pc.pairSection) {                 // arena section full — undo + stop counting past cap
                atomicAdd(pairCount[type], 0xFFFFFFFFu); // -1 (keeps instanceCount clamped)
                atomicAdd(stats[1], 1u);                 // dropped pairs (diag: shadows silently missing)
                continue;
            }
            pairs[type * pc.pairSection + idx] = (slot << 19) | (local & 0x7FFFFu);   // 13-bit slot (static grid = 6144), 19-bit local
            if (!st) { atomicOr(dynUsed[slot], 1u); }
            any_ = true;
            atomicAdd(stats[st ? 3 : 2], 1u);   // diag: dyn vs static pairs
        }
    }
    if (any_ && pc.mode != 1u) atomicAdd(stats[0], 1u);
}
