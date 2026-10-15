#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM page allocation. One thread per virtual page: if it was
// marked NEEDED this frame, claim a physical atlas slot (atomic), write the page
// table entry, and append it to the render list. Per-level counters give a
// distribution diagnostic. PHASE 1B: fresh alloc each frame (no caching yet — the
// page table is cleared to UNMAPPED before this runs). See vk_vsm.cpp.
#include "vsm_common.glsl"

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) buffer Needed    { uint needed[]; };     // per-level page flags (from mark)
layout(set = 0, binding = 1) buffer PageTable { uint pageTable[]; };  // virtual page -> physical slot / UNMAPPED
layout(set = 0, binding = 2) buffer PageList  { uvec4 pageList[]; };  // slot -> (level, px, py, _)
layout(set = 0, binding = 3) buffer AllocInfo { uint allocInfo[]; };  // [0]=count, [1..VSM_LEVELS]=per-level

void main()
{
    uint t = gl_GlobalInvocationID.x;
    if (t >= uint(VSM_PAGE_COUNT)) return;
    if (needed[t] == 0u) return;

    uint slot = atomicAdd(allocInfo[0], 1u);
    if (slot >= uint(VSM_MAX_PHYS)) return;   // atlas full this frame → page stays UNMAPPED

    int level  = int(t) / VSM_PAGES_PER_LVL;
    int within = int(t) % VSM_PAGES_PER_LVL;
    uvec2 page = uvec2(uint(within % VSM_PAGES_AXIS), uint(within / VSM_PAGES_AXIS));

    pageTable[t]   = slot;
    pageList[slot] = uvec4(uint(level), page.x, page.y, 0u);
    atomicAdd(allocInfo[1 + level], 1u);
}
