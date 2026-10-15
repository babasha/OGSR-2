// xrRenderVulkan — Virtual Shadow Maps: shared clipmap mapping (constants + pure
// math, NO resource bindings). Included by vsm_mark.comp and (later) the receiver
// shaders so both agree on world -> light-space -> (level, page, texel). See
// vk_vsm.{h,cpp}. The clipmap is a stack of VSM_LEVELS ortho squares around the
// camera in the sun's light space; each level is 2x the world extent of the finer
// one, all at the same VSM_VIRTUAL_RES. The finest level containing a point wins.
#ifndef VSM_COMMON_GLSL
#define VSM_COMMON_GLSL

const int  VSM_LEVELS        = 6;
const int  VSM_VIRTUAL_RES   = 4096;
const int  VSM_PAGE_SIZE     = 128;
const int  VSM_PAGES_AXIS    = 32;                              // VIRTUAL_RES / PAGE_SIZE
const int  VSM_PAGES_PER_LVL = VSM_PAGES_AXIS * VSM_PAGES_AXIS; // 1024
const int  VSM_PAGE_COUNT    = VSM_LEVELS * VSM_PAGES_PER_LVL;  // 6144 virtual pages
const int  VSM_MAX_PHYS      = 2048;                            // DYNAMIC atlas pages (64x32 grid of 128) — demand-allocated NPC/grass
const int  VSM_ATLAS_W       = 64;                              // dynamic atlas pages across  (W*H = MAX_PHYS)
const int  VSM_ATLAS_H       = 32;                              // dynamic atlas pages down -> 8192x4096 D32
const int  VSM_PAGES_CAP     = 1024;                            // max pages a caster bins into (instanceCount cap)
const int  VSM_GROUP_STRIDE  = 1024;                            // max VSM draw commands per ShadowGPU group region
const uint VSM_UNMAPPED      = 0xFFFFFFFFu;

// STATIC (toroidal-cached) atlas — Phase 1b. Every clipmap virtual page has a fixed
// physical slot via a toroidal map; the atlas is FULLY resident (one slot per virtual page).
const int  VSM_MAX_PHYS_S    = VSM_PAGE_COUNT;                  // 6144 — one slot per virtual page
const int  VSM_ATLAS_W_S     = 64;                              // static atlas grid (W*H = MAX_PHYS_S) -> 8192x12288 D32
const int  VSM_ATLAS_H_S     = 96;

// Flat index into the per-level page arrays (needed[], pageTable[]).
int vsmPageIndex(int level, ivec2 page) {
    return level * VSM_PAGES_PER_LVL + page.y * VSM_PAGES_AXIS + page.x;
}

// Toroidal physical slot for an ABSOLUTE clipmap tile (level + absolute page indices on the
// world-anchored page lattice). Collision-free across the 32x32 window (each visible page maps
// to a unique slot); when the camera scrolls, a page leaving and a page entering share a slot,
// so the entering page evicts the leaving one — the cache's only "eviction" logic.
int vsmToroidalSlot(int level, ivec2 absPage) {
    int sx = absPage.x & (VSM_PAGES_AXIS - 1);   // mod 32 (two's-complement-correct for negatives)
    int sy = absPage.y & (VSM_PAGES_AXIS - 1);
    return level * VSM_PAGES_PER_LVL + sy * VSM_PAGES_AXIS + sx;
}

// Page range on ONE clipmap level covered by a caster's bounding sphere, in light
// space `lp` with radius `R`. Declares lo/hi (unclamped, fractional — vsm_tree_bin's
// acceptPage() weighs a page by them) and p0/p1 (the clamped page rect), and skips
// the level when the sphere misses it entirely.
//
// Every binning pass computes exactly this — static meshes, clusters, skinned, trees
// — and they MUST agree: two passes that disagree on which pages a caster touches put
// its shadow on one page and not the neighbouring one, which reads as a shadow ending
// at a straight line in mid-air. It was copy-pasted into all six loops.
//
// ⚠ A MACRO, not a function, and deliberately so. A function would have to signal
// "skip this level" through a return value and hand back four out-parameters, which
// (a) turns the caller's plain `continue` into a branch on a returned bool and
// (b) reads the whole level vec4 instead of the .xy/.z the math actually uses — the
// SPIR-V diff shows both as real changes (OpPhi/OpSelectionMerge/OpCompositeExtract).
// Textual substitution keeps the emitted code bit-identical to the six copies, so
// this refactor is provably free. USE INSIDE a `for (int L...)` loop over levels.
#define VSM_PAGE_RANGE(L, lp, R, lo, hi, p0, p1)                                            \
    vec2  origin = vsm.level[L].xy;                                                         \
    float pw     = vsm.level[L].z / float(VSM_PAGES_AXIS);                                  \
    vec2  lo = (lp - vec2(R) - origin) / pw;                                                \
    vec2  hi = (lp + vec2(R) - origin) / pw;                                                \
    if (hi.x < 0.0 || hi.y < 0.0 || lo.x >= float(VSM_PAGES_AXIS) || lo.y >= float(VSM_PAGES_AXIS)) \
        continue;                                                                           \
    ivec2 p0 = clamp(ivec2(floor(lo)), ivec2(0), ivec2(VSM_PAGES_AXIS - 1));                \
    ivec2 p1 = clamp(ivec2(floor(hi)), ivec2(0), ivec2(VSM_PAGES_AXIS - 1))

// RECEIVER MASK (r_vsm_rmask, the UE5 VSM idea): per virtual page an 8×8 bitmask of
// which 16×16-texel cells visible receivers actually sample (written by vsm_mark,
// 2 u32 per page: .x = rows 0..3, .y = rows 4..7, bit = row*8+col). Dynamic-pass bins
// drop a (caster, page) pair whose footprint misses every sampled cell — sub-page
// culling of both instances and alpha-test fill. STATIC cached pages must NOT use it
// (a partially-rendered page goes stale when the camera samples other cells later).
// Build the 64-bit rect mask for inclusive cell rect [c0..c1], both in [0,7].
uvec2 vsmRectMask8(ivec2 c0, ivec2 c1) {
    uint row = (0xFFu >> uint(7 - (c1.x - c0.x))) << uint(c0.x);
    uvec2 m = uvec2(0u);
    for (int y = c0.y; y <= c1.y; ++y) {
        if (y < 4) m.x |= row << uint(y * 8);
        else       m.y |= row << uint((y - 4) * 8);
    }
    return m;
}

// Pick the FINEST clipmap level whose ortho square contains light-space XY `lxy`.
// level[L] = (origin.xy = light-space XY of texel (0,0), z = full extent in metres).
// Returns level (0..VSM_LEVELS-1) or -1 if outside every level. On a hit, `uv` is
// the [0,1)^2 position within the level and `page` the page coords [0,VSM_PAGES_AXIS).
int vsmSelect(vec2 lxy, vec4 level[VSM_LEVELS], out vec2 uv, out ivec2 page) {
    for (int L = 0; L < VSM_LEVELS; ++L) {
        vec2 t = (lxy - level[L].xy) / level[L].z;
        if (all(greaterThanEqual(t, vec2(0.0))) && all(lessThan(t, vec2(1.0)))) {
            uv   = t;
            page = ivec2(floor(t * float(VSM_PAGES_AXIS)));
            return L;
        }
    }
    return -1;
}

#endif
