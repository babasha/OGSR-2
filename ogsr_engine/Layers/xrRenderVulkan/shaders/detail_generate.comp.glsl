#version 450
#extension GL_GOOGLE_include_directive : require
#if defined(DETAIL_GEN_LEAN) || defined(DETAIL_CACHE_CULL)
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
#endif
// xrRenderVulkan - GPU grass generation compute shader
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
// SPDX-License-Identifier: MIT
//
// Replaces CPU cache_Decompress + detail_cull.comp:
// Generates grass instances procedurally from slot palette data + heightmap,
// performs frustum/distance culling, and stream-compacts visible instances
// into per-object-type sections of the output SSBO.
//
// Dispatch: one thread per potential grass position in the visible slot range.
// Each thread: lookup slot → palette dither → heightmap Y → cull → output.

#include "hzb_test.glsl"   // HZB_SphereOccluded — shared with world_cull_hzb / tree_cull / lod_cull

// ⭐ PER-INSTANCE PROBE FIELD (r_grass_gi_inst). The forward grass FS used to walk
// the DDGI lattice per FRAGMENT (~16 atlas taps) on the most overdrawn pass in the
// frame — the exact pathology the crown already measured (per-fragment field took
// Trees 2.2 → 11.9 ms) and cured with tree_gi.comp. Grass gets the same cure at
// the rate that fits it: the generator ALREADY runs once per instance per frame,
// so the field is sampled here — once per tuft, a sub-metre object against a 3 m
// lattice — and rides to the vertex shader in the instance record's one spare
// float (color.g, rgb9e5 bits). The EnvLight set (same layout the forward draw
// binds at set 1) carries L + the DDGI bindings; stage flags already include
// COMPUTE (tree_gi.comp opened that door — vk_env_light.cpp).
#define ENV_SET 1
#define DDGI_COMPUTE            // texture() -> textureLod(): compute has no derivatives
#include "light_ubo.glsl"       // L.ddgi_params + L.eye_pos (set 1 b0)
#include "ddgi_field.glsl"      // ddgiFieldWeighted (set 1 b34..36)
#include "rgb9e5.glsl"          // packRGB9E5 — one uint carries the tuft's field sample

layout(local_size_x = 256) in;

// ============================================================================
// Bindings
// ============================================================================

// Heightmap texture (R32F, baked from collision geometry at level load)
layout(set = 0, binding = 0) uniform sampler2D u_Heightmap;

// Slot data (read-only, one entry per slot in the level)
struct GpuSlot {
    float y_base;       // terrain base Y (from DetailSlot::r_ybase())
    float y_height;     // terrain height range
    uint  ids;          // id0:8 | id1:8 | id2:8 | id3:8 (0x3F = empty)
    uint  lighting;     // c_dir:16 | c_hemi:16 (fixed-point 0..65535)
    uint  palette0;     // obj0: a0:8|a1:8|a2:8|a3:8
    uint  palette1;     // obj1: a0:8|a1:8|a2:8|a3:8
    uint  palette2;     // obj2: a0:8|a1:8|a2:8|a3:8
    uint  palette3;     // obj3: a0:8|a1:8|a2:8|a3:8
};

layout(set = 0, binding = 1) readonly buffer SlotData {
    GpuSlot slots[];
} slotBuf;

// Detail object info (read-only, one per object type)
struct DetailObjInfo {
    float minScale;
    float maxScale;
    float bvRadius;
    uint  flags;        // DO_NO_WAVING = 0x0001
};

layout(set = 0, binding = 2) readonly buffer ObjInfo {
    DetailObjInfo objs[];
} objBuf;

// Output: compacted visible instances (same as DetailInstance = 64 bytes)
struct DetailInstance {
    vec4 row0;
    vec4 row1;
    vec4 row2;
    vec4 color;
};

layout(set = 0, binding = 3) writeonly buffer VisibleInstances {
    DetailInstance instances[];
} visInst;

// Atomic counters per object type
layout(set = 0, binding = 4) buffer AtomicCounters {
    uint counters[];
} atomics;

// Indirect draw commands
layout(set = 0, binding = 5) buffer IndirectCommands {
    uint cmds[];
} indirect;

// HZB texture for occlusion culling
layout(set = 0, binding = 6) uniform sampler2D u_HZB;

// (binding 8 used to be u_TrailMap. The CPU still binds a placeholder there —
// the set LAYOUT keeps the slot so it cannot depend on which features a build
// samples — but the shader no longer declares it: the map was a 1×1 black
// dummy with no writer, so the tap it fed multiplied a constant zero into
// every generated instance. See detail.vert for the consumer half.)

// ============================================================================
// Push constants
// ============================================================================
layout(push_constant) uniform PushConstants
{
    mat4 viewProj;              // 64 bytes
    vec4 frustumPlanes[6];      // 96 bytes
    vec4 cameraPos;             // 16 bytes (xyz=pos, w=time)
    vec4 fadeParams;            // 16 bytes (fadeStartSq, fadeLimitSq, fadeRangeSq, density)
    vec4 casterParams;          // 16 bytes (x>0 = CASTER pass: cull radiusSq, no frustum/HZB; y = output capacity override; z = P11 focal 1/tan(fovY/2) for the HZB footprint; w free)
    ivec4 slotRange;            // 16 bytes (minSX, minSZ, countX, countZ)
    // Total: 224 bytes
} pc;

#if defined(DETAIL_CACHE_BAKE) || defined(DETAIL_CACHE_CULL)
// nfs40: the R1-style tuft cache (set 2). Entry per (cell, position): posScale = world position + pre-fade scale,
// meta = (objId or 0xFF = nothing here, yaw bits, slot lighting, radius bits). Header per cell: x/y = slot coord (int
// bits), z/w = terrain y range (z > w = empty slot). Bake list: (cell, sx, sz, 0) for the slots that entered the window.
struct CacheEntry { vec4 posScale; uvec4 meta; };
layout(set = 2, binding = 0) buffer CacheEntries { CacheEntry e[]; } cacheE;
layout(set = 2, binding = 1) buffer CacheHeaders { vec4 h[]; } cacheH;
layout(set = 2, binding = 2) readonly buffer CacheBake { uvec4 b[]; } cacheB;
#endif

// ============================================================================
// Generation UBO (params that don't fit in push constants)
// ============================================================================
layout(set = 0, binding = 7) uniform GenParams
{
    vec4  hmParams;     // (originX, originZ, invScaleX, invScaleZ)
    uvec4 genCounts;    // (totalPositions, numObjTypes, outputCapacity, posPerSlot)
    vec4  dtSlotParams;  // (dtH.offs_x, dtH.offs_z, dtH.size_x, dtH.size_z)
    float slotSize;     // DETAIL_SLOT_SIZE (2.0)
    float detailHeight; // ps_current_detail_height
    uint  hmWidth;
    uint  hmHeight;
} gen;

// ============================================================================
// Hash functions (deterministic RNG replacing CRandom)
// xorshift32: 3 XOR + 2 shifts — ~2x faster than double-mul hash on GPU
// ============================================================================
uint hash(uint x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

uint hash2(uint x, uint y)
{
    uint a = (x << 16) | (y & 0xFFFFu);
    return hash(a ^ 0x12071980u);
}

float hashFloat01(uint h)
{
    return float(h & 0xFFFFu) / 65535.0;
}

// Signed float in [-1, 1]
float hashFloatS(uint h)
{
    return hashFloat01(h) * 2.0 - 1.0;
}

// ============================================================================
// Dither pattern (matches CPU bwdithermap(2, dither))
// ============================================================================
#ifdef DETAIL_GEN_LEAN2
// nfs38: the same 4x4 table as below, packed 4 bits per entry (index 0 in the low nibble) -- no local-memory array.
float getDither(uint col, uint row)
{
    const uint a = ((col & 3u) << 2) | (row & 3u);
    const uint b = (((col >> 2) & 3u) << 2) | ((row >> 2) & 3u);
    const uint ma = (((a < 8u) ? 0x685BD3E0u : 0xA4971F2Cu) >> ((a & 7u) * 4u)) & 15u;
    const uint mb = (((b < 8u) ? 0x685BD3E0u : 0xA4971F2Cu) >> ((b & 7u) * 4u)) & 15u;
    const float magicfact = 254.0 / 16.0;
    return 0.5 + float(ma) * magicfact + (float(mb) / 16.0) * magicfact;
}
#else
float getDither(uint col, uint row)
{
    // Hardcoded 4x4 magic pattern from DetailManager.cpp
    const int m[16] = int[16](0, 14, 3, 13, 11, 5, 8, 6, 12, 2, 15, 1, 7, 9, 4, 10);

    int i = int(col & 3u);
    int j = int(row & 3u);
    int k = int((col >> 2) & 3u);
    int l = int((row >> 2) & 3u);

    // bwdithermap formula: N=255, levels=2, magicfact=(255-1)/16=15.875
    float magicfact = 254.0 / 16.0;
    return 0.5 + float(m[i * 4 + j]) * magicfact
             + (float(m[k * 4 + l]) / 16.0) * magicfact;
}
#endif

// ============================================================================
// Bilinear interpolation of 4 palette corners
// (matches CPU Interpolate() function)
// ============================================================================
float interpolateAlpha(uint palettePacked, uint x, uint z, uint d_size)
{
    // Unpack 4 corners: a0:8|a1:8|a2:8|a3:8
    float a0 = float(palettePacked & 0xFFu);
    float a1 = float((palettePacked >> 8) & 0xFFu);
    float a2 = float((palettePacked >> 16) & 0xFFu);
    float a3 = float((palettePacked >> 24) & 0xFFu);

    float f = float(d_size);
    float fx = float(x) / f;
    float ifx = 1.0 - fx;
    float fz = float(z) / f;
    float ifz = 1.0 - fz;

    float c01 = a0 * ifx + a1 * fx;
    float c23 = a2 * ifx + a3 * fx;
    float c02 = a0 * ifz + a2 * fz;
    float c13 = a1 * ifz + a3 * fz;

    float cx = ifz * c01 + fz * c23;
    float cy = ifx * c02 + fx * c13;
    return (cx + cy) * 0.5;
}

// ============================================================================
// Frustum test: sphere vs 6 planes
// ============================================================================
bool frustumTestSphere(vec3 center, float radius)
{
    for (int i = 0; i < 6; i++)
    {
        float dist = dot(pc.frustumPlanes[i].xyz, center) + pc.frustumPlanes[i].w;
        if (dist < -radius)
            return false;
    }
    return true;
}

// ============================================================================
// Get palette data for a given object slot index (0..3)
// ============================================================================
uint getSlotPalette(GpuSlot s, uint objIdx)
{
    if (objIdx == 0u) return s.palette0;
    if (objIdx == 1u) return s.palette1;
    if (objIdx == 2u) return s.palette2;
    return s.palette3;
}

uint getSlotId(uint ids, uint objIdx)
{
    return (ids >> (objIdx * 8u)) & 0xFFu;
}

// ============================================================================
// Main: generate one grass instance per thread
// ============================================================================
#if !defined(DETAIL_CACHE_BAKE) && !defined(DETAIL_CACHE_CULL)   // nfs40: the cache passes have their own main()
void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= gen.genCounts.x) // totalPositions
        return;

    // Decode: which slot and which position within slot
    uint posPerSlot = gen.genCounts.w;
    uint slotLocal  = gid / posPerSlot;
    uint posLocal   = gid % posPerSlot;

    uint d_size = uint(sqrt(float(posPerSlot - 1u)));  // grid dimension
    // Actually: posPerSlot = (d_size+1)^2, so d_size = sqrt(posPerSlot) - 1
    // But simpler: just pass d_size in genCounts or compute from density
    // For density=0.6, slotSize=2.0: d_size = ceil(2.0/0.6) = 4, posPerSlot = 25
    // So: d_size = 4
    d_size = uint(round(sqrt(float(posPerSlot)) - 1.0));

    uint gridX = posLocal % (d_size + 1u);
    uint gridZ = posLocal / (d_size + 1u);

    // Slot world coordinates
    int sx = pc.slotRange.x + int(slotLocal % uint(pc.slotRange.z));
    int sz = pc.slotRange.y + int(slotLocal / uint(pc.slotRange.z));

    // ---- Slot lookup ----
    // The slot SSBO contains the entire level's slot grid (dtH.size_x × dtH.size_z).
    // Convert world slot coords (sx,sz) to buffer index using dtH offsets from UBO.
    int db_x = sx + int(gen.dtSlotParams.x);  // dtH.offs_x
    int db_z = sz + int(gen.dtSlotParams.y);  // dtH.offs_z
    int dbSizeX = int(gen.dtSlotParams.z);    // dtH.size_x
    int dbSizeZ = int(gen.dtSlotParams.w);    // dtH.size_z

    // Bounds check
    if (db_x < 0 || db_x >= dbSizeX || db_z < 0 || db_z >= dbSizeZ)
        return;

    uint slotIdx = uint(db_z) * uint(dbSizeX) + uint(db_x);
    GpuSlot slot = slotBuf.slots[slotIdx];

    // Check if slot is empty (all 4 IDs = 0x3F)
    uint ID_EMPTY = 0x3Fu;
    uint id0 = getSlotId(slot.ids, 0u);
    uint id1 = getSlotId(slot.ids, 1u);
    uint id2 = getSlotId(slot.ids, 2u);
    uint id3 = getSlotId(slot.ids, 3u);
    if (id0 == ID_EMPTY && id1 == ID_EMPTY && id2 == ID_EMPTY && id3 == ID_EMPTY)
        return;
    // nfs34: per-SLOT rejects, identical for all of its positions and conservative: every position of this slot lies
    // in its 2x2 m rect grown by the jitter, at a terrain height in [y_base - 1, y_base + y_height] (the reject below
    // drops lower ones), and its blade sphere is far smaller than the 3 m margin. Pure early-out (the Switch measured
    // the same tuft count with and without it), so it runs in EVERY generator variant, the full PC one included.
    {
        const float jit = pc.fadeParams.w / 1.7 + 0.01;
        const vec2  lo  = vec2(float(sx), float(sz)) * gen.slotSize - jit;
        const vec2  hi  = lo + gen.slotSize + 2.0 * jit;
        const float limSqS = pc.casterParams.x > 0.0 ? min(pc.casterParams.x, pc.fadeParams.y) : pc.fadeParams.y;
        const vec2  dxz = max(max(lo - pc.cameraPos.xz, pc.cameraPos.xz - hi), vec2(0.0));
        if (dot(dxz, dxz) > limSqS * 1.5)
            return;
        if (pc.casterParams.x <= 0.0) {
            const float ylo = slot.y_base - 1.0, yhi = slot.y_base + max(slot.y_height, 0.0);
            const vec3  c   = vec3(0.5 * (lo + hi), 0.0).xzy;
            const vec3  cc  = vec3(c.x, 0.5 * (ylo + yhi), c.z);
            const float r   = length(vec3(0.5 * (hi.x - lo.x), 0.5 * (yhi - ylo), 0.5 * (hi.y - lo.y))) + 3.0;
            if (!frustumTestSphere(cc, r))
                return;
        }
    }

    // ---- Deterministic RNG (matches CPU hash2 + CRandom) ----
    uint seed = hash2(uint(sx) & 0xFFFFu, uint(sz) & 0xFFFFu);
    // Per-position seed
    uint posSeed = hash(seed ^ (gridX * 7u + gridZ * 13u + posLocal * 31u));

    // Jitter shifts (matches CPU r_jitter.randI(16))
    uint shift_x = hash(posSeed ^ 0xA100u) & 15u;
    uint shift_z = hash(posSeed ^ 0xA200u) & 15u;

    // ---- Palette dither: select which object to place ----
    // InterpolateAndDither for each of 4 object slots
#ifdef DETAIL_GEN_LEAN2
    // nfs38: no arrays (NAK put sel[4]/ids[4] and the dither table in local memory). The dither threshold
    // depends on the position only, so it is computed once; the selected types are a bit mask, and the r-th
    // selected one (same order as the loop below) is found by clearing the lowest bit r times.
    const float threshold = getDither((gridX + shift_x) & 15u, (gridZ + shift_z) & 15u);
    const uint  gxA = min(gridX, d_size - 1u), gzA = min(gridZ, d_size - 1u);
    uint selMask = 0u, selCount = 0u;
    for (uint i = 0u; i < 4u; ++i) {
        if (((slot.ids >> (i * 8u)) & 0xFFu) == ID_EMPTY) continue;
        const uint pal = (i == 0u) ? slot.palette0 : (i == 1u) ? slot.palette1 : (i == 2u) ? slot.palette2 : slot.palette3;
        const int  c   = clamp(int(interpolateAlpha(pal, gxA, gzA, d_size) + 0.5), 0, 255);
        if (float(c) > threshold) { selMask |= 1u << i; ++selCount; }
    }
    if (selCount == 0u) return;
    uint pick = selMask;
    if (selCount > 1u) {
        const uint r = hash(posSeed ^ 0xB100u) % selCount;
        for (uint k = 0u; k < r; ++k) pick &= pick - 1u;
    }
    const uint chosenSlotIdx = uint(findLSB(pick));
    uint objId = (slot.ids >> (chosenSlotIdx * 8u)) & 0xFFu;
#else
    bool sel[4];
    uint selCount = 0u;
    uint ids[4] = uint[4](id0, id1, id2, id3);

    for (int i = 0; i < 4; i++)
    {
        sel[i] = false;
        if (ids[i] == ID_EMPTY) continue;

        float alpha = interpolateAlpha(getSlotPalette(slot, uint(i)),
                                       min(gridX, d_size - 1u),
                                       min(gridZ, d_size - 1u),
                                       d_size);
        // Round to int and clamp (matches CPU: iFloor(alpha + .5f), clamp 0-255)
        int c = int(alpha + 0.5);
        c = clamp(c, 0, 255);

        uint row = (gridZ + shift_z) & 15u;
        uint col = (gridX + shift_x) & 15u;
        float threshold = getDither(col, row);

        if (float(c) > threshold)
        {
            sel[i] = true;
            selCount++;
        }
    }

    if (selCount == 0u) return;

    // Select one from candidates (matches CPU r_selection.randI)
    uint chosenSlotIdx = 0u;
    if (selCount == 1u)
    {
        for (int i = 0; i < 4; i++)
            if (sel[i]) { chosenSlotIdx = uint(i); break; }
    }
    else
    {
        uint r = hash(posSeed ^ 0xB100u) % selCount;
        uint cnt = 0u;
        for (int i = 0; i < 4; i++)
        {
            if (sel[i])
            {
                if (cnt == r) { chosenSlotIdx = uint(i); break; }
                cnt++;
            }
        }
    }

    uint objId = ids[chosenSlotIdx];
#endif
    if (objId >= gen.genCounts.y) // numObjTypes
        return;

    DetailObjInfo obj = objBuf.objs[objId];

    // ---- World position (XZ) ----
    float slotMinX = float(sx) * gen.slotSize;
    float slotMinZ = float(sz) * gen.slotSize;

    float density = pc.fadeParams.w;
    float jitter = density / 1.7;

    float rx = (float(gridX) / float(d_size)) * gen.slotSize + slotMinX;
    float rz = (float(gridZ) / float(d_size)) * gen.slotSize + slotMinZ;

    // Apply jitter
    rx += hashFloatS(hash(posSeed ^ 0xC100u)) * jitter;
    rz += hashFloatS(hash(posSeed ^ 0xC200u)) * jitter;

    // ---- Distance culling (early out) ----
    // CASTER pass (casterParams.x > 0): the shadow-caster radius replaces the
    // visual fade limit — shadow casters only matter near the camera, and a
    // blade past the visual fade shouldn't cast at all (min with fadeLimit).
    float limSq = pc.casterParams.x > 0.0 ? min(pc.casterParams.x, pc.fadeParams.y)
                                          : pc.fadeParams.y;
    vec3 delta = vec3(rx, 0.0, rz) - pc.cameraPos.xyz;
    float distSqXZ = delta.x * delta.x + delta.z * delta.z;
    if (distSqXZ > limSq * 1.5) // limit with some margin for Y
        return;

    // nfs25: distance thinning (visible pass only) -- every tuft inside casterParams.w metres, past it a stable
    // (r0/d)^2 share, the survivors a little larger.
    float thinScale = 1.0;
    if (pc.casterParams.w > 0.0 && pc.casterParams.x <= 0.0) {
        const float r0sq = pc.casterParams.w * pc.casterParams.w;
        if (distSqXZ > r0sq) {
            const float keep = max(r0sq / distSqXZ, 0.08);
            if (hashFloat01(hash(posSeed ^ 0x7A11u)) > keep)
                return;
            thinScale = min(pow(keep, -0.25), 1.6);
        }
    }

    // ---- Get Y position ----
    // Heightmap stores MIN Y (ground level, ignoring bridges/roofs).
    // Fallback to slot.y_base if heightmap has no data.

    float hmU = (rx - gen.hmParams.x) * gen.hmParams.z; // (rx - originX) * invScaleX
    float hmV = (rz - gen.hmParams.y) * gen.hmParams.w; // (rz - originZ) * invScaleZ
    // Conservative ground height: bilinear blends an ELEVATED neighbour texel
    // in at ledges/foundations and floats the grass mid-air — take the MIN of
    // the 2x2 footprint instead (slight root sinking on slopes is invisible,
    // floating tufts are not). Also keeps one sentinel(9000) neighbour from
    // poisoning the sample at heightmap hole borders.
    vec4 hmG = textureGather(u_Heightmap, vec2(hmU, hmV), 0);
    float hmY = min(min(hmG.x, hmG.y), min(hmG.z, hmG.w));

    float terrainY;
    if (hmY < 9000.0 && hmY > -9000.0)
    {
        // Valid heightmap data — use ground-level height
        terrainY = hmY;
    }
    else
    {
        // No data — fall back to slot.y_base
        terrainY = slot.y_base;
    }

    // R4 parity: cache_Decompress rejects hits below the slot's authored Y
    // range (y < vis.box.min.y). The min-Y heightmap lands on INTERIOR floors
    // below the painted terrain (bar/basement interiors) — reject so grass
    // never grows inside buildings. 1 m epsilon covers the 0.2 m y_base
    // quantisation plus bilinear heightmap error on slopes.
    if (terrainY < slot.y_base - 1.0)
        return;

    vec3 worldPos = vec3(rx, terrainY, rz);

    // ---- Full distance culling with Y ----
    vec3 fullDelta = worldPos - pc.cameraPos.xyz;
    float distSq = dot(fullDelta, fullDelta);
    if (distSq > limSq)
        return;

    // ---- Scale ----
    float scale = mix(obj.minScale * 0.5, obj.maxScale * 0.9,
                      hashFloat01(hash(posSeed ^ 0xD100u)));
    scale *= gen.detailHeight; // ps_current_detail_height
    scale *= thinScale;        // nfs25

    // ---- Frustum culling (VISIBLE pass only — a caster behind the camera can
    // still throw its shadow INTO the view: campfire dapples on the wall you
    // walk toward, sun shadows reaching into the frustum) ----
    float radius = obj.bvRadius * scale;
    if (pc.casterParams.x <= 0.0 && !frustumTestSphere(worldPos, radius))
        return;

    // ---- HZB occlusion culling ----
    // Only cull the FAR band. Near grass sits ON the terrain the HZB is built
    // from (co-planar), so testing it just flickers at the cull boundary — and
    // near grass is sparse in instance count, so culling it saves little.
    // Occlusion pays off on the far band where area (∝ r²) packs most instances.
    // Within HZB_NEAR_SKIP metres we keep everything; beyond it the shared test
    // compares the grass's NEAR face (base pulled toward the camera by its
    // radius), so a small heightmap-vs-rendered-mesh mismatch cannot cull
    // surface-grazing grass.
    //
    // ⭐The footprint maths now lives ONCE, in hzb_test.glsl, shared with
    // world_cull_hzb / tree_cull / lod_cull. Grass was the last consumer still
    // carrying a private copy, and that copy had missed BOTH fixes the others
    // received: no focal (P11) scale on the footprint, understating it ~1.5-1.9x
    // vertically, and one UV radius used on both axes even though UV is
    // anisotropic. Both errors SHRINK the sampled block, so the MAX is taken over
    // less area than the blade actually covers, reads "there is something nearer
    // in front of me", and culls grass that is in plain view. That is the patch
    // flicker HZB_NEAR_SKIP was raised to hide.
    // ⚠HZB_NEAR_SKIP is left at 25 m on purpose: lowering it changes the PICTURE
    // (more far grass culled) and belongs to its own measured A/B, not to this
    // correctness fix.
#ifndef DETAIL_GEN_LEAN2   // nfs38: no grass pyramid on the Switch -- the whole test compiled out
    const float HZB_NEAR_SKIP = 25.0;
    float camDist = distance(pc.cameraPos.xyz, worldPos);
    if (camDist > HZB_NEAR_SKIP && pc.casterParams.x <= 0.0   // caster pass: occluded grass still casts
#ifdef DETAIL_GEN_LEAN
        && pc.casterParams.z > 0.0   // nfs34: focal 0 = the C++ side did not build the pyramid this frame
#endif
        && HZB_SphereOccluded(u_HZB, pc.viewProj, worldPos, radius,
                              pc.cameraPos.xyz, pc.casterParams.z /* P11 focal */))
        return;
#endif

    // ---- Fade alpha (scale-based, matches detail_cull.comp) ----
    float alpha = 1.0;
    if (distSq > pc.fadeParams.x) // fadeStartSq
    {
        alpha = 1.0 - (distSq - pc.fadeParams.x) / pc.fadeParams.z; // fadeRangeSq
        alpha = clamp(alpha, 0.0, 1.0);
    }

    float fadedScale = scale * alpha;
    if (fadedScale < 0.01)
        return;

    // ---- Build transform matrix (rotation Y + scale + translate) ----
    float yaw = hashFloat01(hash(posSeed ^ 0xE100u)) * 6.2831853; // 0..2π
    float cs = cos(yaw);
    float sn = sin(yaw);

    // RotateY * Scale matrix + translate
    // [ cs*s  0  sn*s  tx ]
    // [  0    s   0    ty ]
    // [-sn*s  0  cs*s  tz ]
    float s = fadedScale;

    // ---- Lighting from slot data ----
    float c_dir  = float(slot.lighting & 0xFFFFu) / 65535.0;
    float c_hemi = float((slot.lighting >> 16) & 0xFFFFu) / 65535.0;

    // ---- Atomic append to per-obj-type section ----
    // Caster pass writes into the (smaller) caster SSBO — its capacity comes
    // via push (casterParams.y) since the UBO holds the visible pass's.
    uint outputCapacity = pc.casterParams.y > 0.0 ? uint(pc.casterParams.y + 0.5)
                                                  : gen.genCounts.z;
    uint numObjTypes = gen.genCounts.y;
    uint sectionSize = outputCapacity / max(numObjTypes, 1u);

#ifdef DETAIL_GEN_LEAN
    // nfs34: subgroup-aggregated append -- the lanes of one object type share one atomicAdd (a warp holds a few types).
    uint localIdx = 0u;
    for (;;) {
        const uint lead = subgroupBroadcastFirst(objId);
        if (lead == objId) {
            const uvec4 m    = subgroupBallot(true);
            const uint  n    = subgroupBallotBitCount(m);
            const uint  rank = subgroupBallotExclusiveBitCount(m);
            uint base = 0u;
            if (subgroupElect()) base = atomicAdd(atomics.counters[objId], n);
            localIdx = subgroupBroadcastFirst(base) + rank;
            break;
        }
    }
#else
    uint localIdx = atomicAdd(atomics.counters[objId], 1u);
#endif

    if (localIdx >= sectionSize)
    {
        // Over capacity — undo the increment so counter stays at sectionSize
        // (prevents inflated instanceCount in indirect draw). Count the drop:
        // silent per-type overflow = whole grass regions missing (holes) —
        // slots [64..127] (GPU_MAX_OBJ_TYPES + objId) are read back and logged.
        atomicAdd(atomics.counters[objId], 0xFFFFFFFFu); // -1
        atomicAdd(atomics.counters[64u + objId], 1u);
        return;
    }

    uint outIdx = objId * sectionSize + localIdx;
    if (outIdx >= outputCapacity)
        return;

    // ---- Per-instance probe field (see the include banner above) ----
    // Sampled at the tuft's mid-height, N straight up, exactly the question the
    // fragment used to ask per pixel (a blade has no normal). The CASTER pass
    // skips it — depth-only consumers never shade.
    // ⚠ f.w (the volume-fade weight) is DROPPED on purpose: the fragment rebuilds
    // it from the ddgi UBO at the blade's own position, which keeps the volume
    // edge crossfading per-pixel smooth AND folds in the second fade the old
    // path computed separately for the flat-ambient gate.
    uint giPacked = 0u;
#ifndef DETAIL_GEN_LEAN   // nfs34: the Switch R1 grass never reads the per-tuft field
    if (pc.casterParams.x <= 0.0) {
        const vec3 giP = worldPos + vec3(0.0, radius * 0.5, 0.0);
        const vec4 f = ddgiFieldWeighted(giP, vec3(0.0, 1.0, 0.0),
                                         normalize(pc.cameraPos.xyz - giP));
        giPacked = packRGB9E5(f.rgb);
    }
#endif

    // ---- Write output instance ----
    // color.g carries the packed field sample (was reserved 0 after the trail
    // press-down died — see the binding-8 note above). uintBitsToFloat is a
    // bitcast: the store writes the rgb9e5 bits; the vertex shader reads them
    // back through an R32_UINT attribute view of the same bytes, so no float
    // operation ever touches the payload.
    DetailInstance inst;
    inst.row0 = vec4( cs * s, 0.0,    sn * s, worldPos.x);
    inst.row1 = vec4( 0.0,    s,      0.0,    worldPos.y);
    inst.row2 = vec4(-sn * s, 0.0,    cs * s, worldPos.z);
    inst.color = vec4(c_dir, uintBitsToFloat(giPacked), float(objId), c_hemi);

    visInst.instances[outIdx] = inst;
}
#endif

#if defined(DETAIL_CACHE_BAKE)
// nfs40 bake: one thread per (listed slot, position); pc.slotRange = (list offset, list count, W, -).
void main()
{
    const uint gid = gl_GlobalInvocationID.x;
    const uint posPerSlot = gen.genCounts.w;
    const uint item = gid / posPerSlot, posLocal = gid % posPerSlot;
    if (item >= uint(pc.slotRange.y)) return;
    const uvec4 job = cacheB.b[uint(pc.slotRange.x) + item];
    const uint cell = job.x;
    const int sx = int(job.y), sz = int(job.z);
    const uint outE = cell * posPerSlot + posLocal;
    cacheE.e[outE].meta.x = 0xFFu;   // nothing here unless a tuft is produced below

    uint d_size = uint(round(sqrt(float(posPerSlot)) - 1.0));
    uint gridX = posLocal % (d_size + 1u);
    uint gridZ = posLocal / (d_size + 1u);
    int db_x = sx + int(gen.dtSlotParams.x);
    int db_z = sz + int(gen.dtSlotParams.y);
    const bool inDb = !(db_x < 0 || db_x >= int(gen.dtSlotParams.z) || db_z < 0 || db_z >= int(gen.dtSlotParams.w));
    GpuSlot slot;
    if (inDb) slot = slotBuf.slots[uint(db_z) * uint(gen.dtSlotParams.z) + uint(db_x)];
    uint ID_EMPTY = 0x3Fu;
    const bool empty = !inDb || ((slot.ids & 0xFFu) == ID_EMPTY && ((slot.ids >> 8) & 0xFFu) == ID_EMPTY
                                 && ((slot.ids >> 16) & 0xFFu) == ID_EMPTY && ((slot.ids >> 24) & 0xFFu) == ID_EMPTY);
    if (posLocal == 0u)
        cacheH.h[cell] = empty ? vec4(intBitsToFloat(sx), intBitsToFloat(sz), 1e30, -1e30)
                               : vec4(intBitsToFloat(sx), intBitsToFloat(sz), slot.y_base - 1.0, slot.y_base + max(slot.y_height, 0.0));
    if (empty) return;

    uint seed = hash2(uint(sx) & 0xFFFFu, uint(sz) & 0xFFFFu);
    uint posSeed = hash(seed ^ (gridX * 7u + gridZ * 13u + posLocal * 31u));
    uint shift_x = hash(posSeed ^ 0xA100u) & 15u;
    uint shift_z = hash(posSeed ^ 0xA200u) & 15u;
    const float threshold = getDither((gridX + shift_x) & 15u, (gridZ + shift_z) & 15u);
    const uint  gxA = min(gridX, d_size - 1u), gzA = min(gridZ, d_size - 1u);
    uint selMask = 0u, selCount = 0u;
    for (uint i = 0u; i < 4u; ++i) {
        if (((slot.ids >> (i * 8u)) & 0xFFu) == ID_EMPTY) continue;
        const uint pal = (i == 0u) ? slot.palette0 : (i == 1u) ? slot.palette1 : (i == 2u) ? slot.palette2 : slot.palette3;
        const int  c   = clamp(int(interpolateAlpha(pal, gxA, gzA, d_size) + 0.5), 0, 255);
        if (float(c) > threshold) { selMask |= 1u << i; ++selCount; }
    }
    if (selCount == 0u) return;
    uint pick = selMask;
    if (selCount > 1u) {
        const uint r = hash(posSeed ^ 0xB100u) % selCount;
        for (uint k = 0u; k < r; ++k) pick &= pick - 1u;
    }
    const uint objId = (slot.ids >> (uint(findLSB(pick)) * 8u)) & 0xFFu;
    if (objId >= gen.genCounts.y) return;
    DetailObjInfo obj = objBuf.objs[objId];

    float slotMinX = float(sx) * gen.slotSize;
    float slotMinZ = float(sz) * gen.slotSize;
    float jitter = pc.fadeParams.w / 1.7;
    float rx = (float(gridX) / float(d_size)) * gen.slotSize + slotMinX;
    float rz = (float(gridZ) / float(d_size)) * gen.slotSize + slotMinZ;
    rx += hashFloatS(hash(posSeed ^ 0xC100u)) * jitter;
    rz += hashFloatS(hash(posSeed ^ 0xC200u)) * jitter;

    float hmU = (rx - gen.hmParams.x) * gen.hmParams.z;
    float hmV = (rz - gen.hmParams.y) * gen.hmParams.w;
    vec4 hmG = textureGather(u_Heightmap, vec2(hmU, hmV), 0);
    float hmY = min(min(hmG.x, hmG.y), min(hmG.z, hmG.w));
    float terrainY = (hmY < 9000.0 && hmY > -9000.0) ? hmY : slot.y_base;
    if (terrainY < slot.y_base - 1.0) return;

    float scale = mix(obj.minScale * 0.5, obj.maxScale * 0.9, hashFloat01(hash(posSeed ^ 0xD100u)));
    scale *= gen.detailHeight;
    const float radius = obj.bvRadius * scale;
    const float yaw = hashFloat01(hash(posSeed ^ 0xE100u)) * 6.2831853;
    cacheE.e[outE].posScale = vec4(rx, terrainY, rz, scale);
    cacheE.e[outE].meta = uvec4(objId, floatBitsToUint(yaw), slot.lighting, floatBitsToUint(radius));
}
#elif defined(DETAIL_CACHE_CULL)
// nfs40 cull: one thread per (window slot, position), as the generator; reads the cached tuft instead of making it.
void main()
{
    const uint gid = gl_GlobalInvocationID.x;
    if (gid >= gen.genCounts.x) return;
    const uint posPerSlot = gen.genCounts.w;
    const uint slotLocal = gid / posPerSlot, posLocal = gid % posPerSlot;
    const int W = pc.slotRange.z;
    const int sx = pc.slotRange.x + int(slotLocal % uint(W));
    const int sz = pc.slotRange.y + int(slotLocal / uint(W));
    const uint cell = uint(((sx % W) + W) % W) + uint(((sz % W) + W) % W) * uint(W);
    const vec4 hd = cacheH.h[cell];
    if (floatBitsToInt(hd.x) != sx || floatBitsToInt(hd.y) != sz || hd.z > hd.w) return;   // not baked / empty slot
    {   // slot pre-cull (as the lean generator)
        const float jit = pc.fadeParams.w / 1.7 + 0.01;
        const vec2  lo  = vec2(float(sx), float(sz)) * gen.slotSize - jit;
        const vec2  hi  = lo + gen.slotSize + 2.0 * jit;
        const vec2  dxz = max(max(lo - pc.cameraPos.xz, pc.cameraPos.xz - hi), vec2(0.0));
        if (dot(dxz, dxz) > pc.fadeParams.y * 1.5) return;
        const vec3  cc  = vec3(0.5 * (lo.x + hi.x), 0.5 * (hd.z + hd.w), 0.5 * (lo.y + hi.y));
        const float r   = length(vec3(0.5 * (hi.x - lo.x), 0.5 * (hd.w - hd.z), 0.5 * (hi.y - lo.y))) + 3.0;
        if (!frustumTestSphere(cc, r)) return;
    }
    const CacheEntry ce = cacheE.e[cell * posPerSlot + posLocal];
    const uint objId = ce.meta.x;
    if (objId == 0xFFu) return;
    const vec3  worldPos = ce.posScale.xyz;
    const float scale    = ce.posScale.w;
    const float limSq    = pc.fadeParams.y;
    const vec2  dXZ      = worldPos.xz - pc.cameraPos.xz;
    if (dot(dXZ, dXZ) > limSq * 1.5) return;
    const vec3  fullDelta = worldPos - pc.cameraPos.xyz;
    const float distSq    = dot(fullDelta, fullDelta);
    if (distSq > limSq) return;
    if (!frustumTestSphere(worldPos, uintBitsToFloat(ce.meta.w))) return;
    float alpha = 1.0;
    if (distSq > pc.fadeParams.x) alpha = clamp(1.0 - (distSq - pc.fadeParams.x) / pc.fadeParams.z, 0.0, 1.0);
    const float s = scale * alpha;
    if (s < 0.01) return;
    const float yaw = uintBitsToFloat(ce.meta.y);
    const float cs = cos(yaw), sn = sin(yaw);
    const float c_dir  = float(ce.meta.z & 0xFFFFu) / 65535.0;
    const float c_hemi = float((ce.meta.z >> 16) & 0xFFFFu) / 65535.0;

    const uint outputCapacity = gen.genCounts.z;
    const uint sectionSize = outputCapacity / max(gen.genCounts.y, 1u);
    uint localIdx = 0u;
    for (;;) {
        const uint lead = subgroupBroadcastFirst(objId);
        if (lead == objId) {
            const uvec4 m    = subgroupBallot(true);
            const uint  n    = subgroupBallotBitCount(m);
            const uint  rank = subgroupBallotExclusiveBitCount(m);
            uint base = 0u;
            if (subgroupElect()) base = atomicAdd(atomics.counters[objId], n);
            localIdx = subgroupBroadcastFirst(base) + rank;
            break;
        }
    }
    if (localIdx >= sectionSize) {
        atomicAdd(atomics.counters[objId], 0xFFFFFFFFu);
        atomicAdd(atomics.counters[64u + objId], 1u);
        return;
    }
    const uint outIdx = objId * sectionSize + localIdx;
    if (outIdx >= outputCapacity) return;
    DetailInstance inst;
    inst.row0 = vec4( cs * s, 0.0,    sn * s, worldPos.x);
    inst.row1 = vec4( 0.0,    s,      0.0,    worldPos.y);
    inst.row2 = vec4(-sn * s, 0.0,    cs * s, worldPos.z);
    inst.color = vec4(c_dir, 0.0, float(objId), c_hemi);
    visInst.instances[outIdx] = inst;
}
#endif
