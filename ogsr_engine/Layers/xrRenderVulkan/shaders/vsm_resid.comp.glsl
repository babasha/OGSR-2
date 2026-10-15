#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM STATIC-atlas toroidal residency (Phase 1b). One thread per virtual
// clipmap page. If the page is needed this frame, map it to its fixed toroidal physical slot
// (no allocator/hash/free-list — the modulo IS the eviction), and decide whether it must be
// (re)rendered this frame:
//   * the slot currently holds a DIFFERENT absolute tile  → camera scrolled it in → DIRTY
//   * round-robin refresh while the sun is moving          → keep the cached depth fresh → DIRTY
// otherwise the page is reused as-is (cache hit). Dirty slots are compacted into dirtyList for
// the per-page clear + only-dirty caster render. The window->slot map within a level's 32x32
// window is a bijection, so each touched slot has exactly ONE writer thread (no atomics on
// physTile/slotDirty). See vk_vsm.cpp.
#include "vsm_common.glsl"

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) readonly buffer Needed    { uint  needed[]; };     // window page -> needed flag (from MARK)
layout(set = 0, binding = 1) buffer PageTable          { uint  pageTable[]; };  // window page -> static slot / UNMAPPED
layout(set = 0, binding = 2) buffer PageList           { uvec4 pageList[]; };   // slot -> (level, px, py, _) this frame (render uses it)
layout(set = 0, binding = 3) buffer PhysTile           { uvec2 physTile[]; };   // slot -> (absX, absY) it holds (PERSISTENT across frames)
layout(set = 0, binding = 4) buffer SlotDirty          { uint  slotDirty[]; };  // slot -> 1 if it must render this frame (cleared each frame)
layout(set = 0, binding = 5) buffer DirtyList          { uint  dirtyList[]; };  // compact dirty slots (clear quad + diagnostics)
layout(set = 0, binding = 6) buffer DrawClear          { uint  drawClear[]; };  // VkDrawIndirectCommand: [0]=vtx(6) [1]=instanceCount=dirtyCount [2]=0 [3]=0
layout(set = 0, binding = 7) buffer PriorValid         { uint  priorValid[]; }; // slot -> 1 if its CACHED depth is this world tile's (physTile matched) → valid shadow-HZB occluder (r_vsm_hzb)
layout(set = 0, binding = 8) readonly buffer Hits      { uint  pageHits[]; };   // per-page sampled-pixel count from the mark (r_vsm_gaze)

layout(push_constant) uniform Push {
    ivec4 pageBase[3];   // per-level window first-page absolute index: [L>>1].xy if L even, .zw if odd
    uint  frame;
    uint  refreshN;      // round-robin period (refresh slot when slot%N == frame%N); 0 = disabled
    uint  sunMoving;     // 1 = sun rotated this frame → enable round-robin refresh
    uint  forceDirty;    // 1 = mark every needed page dirty (cache OFF baseline: render all visible)
    vec4  inval[4];      // invalidation circles, LIGHT-space: xy = center, z = radius (m), z<=0 = unused;
                         // [0].w = L0 page width (m) → pw(L) = [0].w * 2^L. Fed by tree near/far
                         // transitions (r_vsm_tree_wind): pages a crossing tree overlaps re-render
                         // so the static atlas adds/removes its rigid shadow the SAME frame.
                         // [1].w = dirty budget (r_vsm_dirty_budget, pages/frame, 0 = unlimited):
                         // scrolled-in (wrong-tile) pages over budget DEFER to a later frame — the
                         // UE5 DeferredInvalidationBudget idea against the toroidal-scroll redraw
                         // spikes. The push is at the 128 B limit, so the knob rides a free .w.
                         // [2].w = GAZE budget (r_vsm_gaze_pages, pages/frame, 0 = gaze OFF).
                         // [3].w = GAZE full-rate hits (r_vsm_gaze_px): pages sampled by >= this
                         // many mark samples re-render EVERY frame; cadence scales inversely below.
} pc;

ivec2 pageBaseOf(int L) { ivec4 v = pc.pageBase[L >> 1]; return ((L & 1) == 0) ? v.xy : v.zw; }

void main()
{
    uint vp = gl_GlobalInvocationID.x;
    if (vp >= uint(VSM_PAGE_COUNT)) return;
    if (needed[vp] == 0u) { pageTable[vp] = VSM_UNMAPPED; return; }

    int   level   = int(vp) / VSM_PAGES_PER_LVL;
    int   within  = int(vp) % VSM_PAGES_PER_LVL;
    ivec2 wpage   = ivec2(within % VSM_PAGES_AXIS, within / VSM_PAGES_AXIS);   // window page coords
    ivec2 absPage = pageBaseOf(level) + wpage;                                 // absolute tile on the world lattice

    int slot = vsmToroidalSlot(level, absPage);
    pageTable[vp]  = uint(slot);
    pageList[slot] = uvec4(uint(level), uint(wpage.x), uint(wpage.y), 0u);     // this frame's window coords for the render

    uvec2 tile  = uvec2(uint(absPage.x), uint(absPage.y));
    bool  wrong = (physTile[slot] != tile);   // slot holds a different tile → scrolled in (new/evicted)
    // shadow-HZB (r_vsm_hzb): the slot's cached depth is a valid occluder for THIS page iff it already
    // held this world tile (!wrong). Scrolled-in slots hold a different tile → their depth must NOT be
    // used to cull. (Written before physTile is updated below; the reduce reads it for dirty slots.)
    priorValid[slot] = wrong ? 0u : 1u;
    bool  refresh = (pc.sunMoving != 0u) && (pc.refreshN != 0u) &&
                    ((uint(slot) % pc.refreshN) == (pc.frame % pc.refreshN));
    // GAZE refresh (r_vsm_gaze): while the sun moves, pages the player is actually
    // looking at re-render on a cadence ∝ their on-screen footprint (pageHits from
    // the mark). A page filling >= [3].w samples refreshes EVERY frame (its shadow
    // glides as smoothly as the dynamic atlas); smaller/farther pages refresh every
    // ceil(H1/hits) frames (slot-staggered phase); beyond 16 frames the plain
    // round-robin above is the better owner. Distant shadows self-deprioritize:
    // perspective shrinks their footprint, and their coarse-level texels quantize
    // the motion anyway. Budget-capped by a second dirtyList tail counter
    // (index MAX+1; the wrong-tile counter owns MAX).
    if (!refresh && pc.sunMoving != 0u && pc.inval[2].w > 0.0) {
        uint h = pageHits[vp];
        if (h > 0u) {
            float pF = ceil(max(pc.inval[3].w, 1.0) / float(h));
            if (pF <= 16.0) {
                uint period = uint(max(pF, 1.0));
                if (((pc.frame + uint(slot)) % period) == 0u) {
                    uint g = atomicAdd(dirtyList[uint(VSM_MAX_PHYS_S) + 1u], 1u);
                    if (g < uint(pc.inval[2].w + 0.5)) refresh = true;
                }
            }
        }
    }
    // Invalidation circles (tree near/far transitions): the page's world-anchored
    // light-space rect vs each circle → force a re-render.
    bool inval = false;
    {
        float pw = pc.inval[0].w * float(1 << level);   // page width (m) at this level
        if (pw > 0.0) {
            vec2 pmin = vec2(absPage) * pw, pmax = pmin + vec2(pw);
            for (int i = 0; i < 4; ++i) {
                float r = pc.inval[i].z;
                if (r <= 0.0) continue;
                vec2 d = clamp(pc.inval[i].xy, pmin, pmax) - pc.inval[i].xy;
                if (dot(d, d) <= r * r) { inval = true; break; }
            }
        }
    }
    // DIRTY BUDGET (r_vsm_dirty_budget): the 7+ ms VSMrender spikes are toroidal-scroll
    // eviction bursts — dozens of wrong-tile pages all re-rendering the same frame. Cap
    // them: wrong-tile pages over the per-frame budget DEFER (page stays UNMAPPED this
    // frame → receivers fall back to the next coarser mapped level, vsm_resolve; physTile
    // keeps the OLD tile so the page competes again next frame until it wins a slot).
    // NOT budgeted: inval circles (tree static↔dyn transitions MUST land the same frame —
    // ghosts/double shadows otherwise), forceDirty (cache-off baseline), and round-robin
    // refresh (already rate-limited by refreshN; its content stays valid, just staler).
    // Counter = the TAIL dword of dirtyList (index VSM_MAX_PHYS_S; appends below are
    // guarded < VSM_MAX_PHYS_S so they never touch it; zeroed in the frame-start fill,
    // read back for the throttle/dirty telemetry). NOT drawClear[2]/[3] — those are the
    // live clear draw's firstVertex/firstInstance. Counted even with the budget off (diag).
    if (wrong && !inval && pc.forceDirty == 0u) {
        uint budget = uint(pc.inval[1].w + 0.5);
        uint n = atomicAdd(dirtyList[uint(VSM_MAX_PHYS_S)], 1u);
        if (budget != 0u && n >= budget) { pageTable[vp] = VSM_UNMAPPED; return; }
    }
    if (wrong || refresh || inval || pc.forceDirty != 0u) {
        physTile[slot]  = tile;
        slotDirty[slot] = 1u;
        uint d = atomicAdd(drawClear[1], 1u);     // instanceCount of the clear draw = dirty count
        if (d < uint(VSM_MAX_PHYS_S)) dirtyList[d] = uint(slot);
    }
}
