#version 450
// xrRenderVulkan - Hierarchical Z-Buffer build compute shader
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
// SPDX-License-Identifier: MIT
//
// Builds a mip chain for the HZB (Hierarchical Z-Buffer) used by the grass
// generation compute shader for occlusion culling.
// Each mip level contains the MAX depth of the 2x2 region from the source.
//
// Pass 0 (isFirstPass=1): reads from the scene depth buffer, writes HZB mip 0.
// Pass N (isFirstPass=0): reads HZB mip N-1 (selected via srcMip), writes mip N.
//
// Dispatched once per mip level with appropriate push constants. MAX reduction
// is conservative: an instance is occluded only when it is farther than the
// farthest surface in its region, so culling never drops visible grass.

layout(local_size_x = 8, local_size_y = 8) in;

// ============================================================================
// Push constants
// ============================================================================
layout(push_constant) uniform HZBBuildConstants
{
    ivec2 srcSize;      // Source mip dimensions
    ivec2 dstSize;      // Destination mip dimensions
    uint  srcMip;       // Source mip level (depth=0 on first pass)
    uint  dstMip;       // Destination mip level
    uint  isFirstPass;  // 1 = reading from depth buffer, 0 = reading from HZB
    uint  _pad;
} pc;

// ============================================================================
// Bindings
// ============================================================================
// Source: either the depth buffer (first pass) or the previous HZB mip.
layout(set = 0, binding = 0) uniform sampler2D uSrcDepth;

// Destination: current HZB mip level (storage image).
layout(set = 0, binding = 1, r32f) writeonly uniform image2D uDstMip;

// ============================================================================
// Main
// ============================================================================
void main()
{
    ivec2 dstCoord = ivec2(gl_GlobalInvocationID.xy);
    if (dstCoord.x >= pc.dstSize.x || dstCoord.y >= pc.dstSize.y)
        return;

    // Map dst pixel to its source block. With ODD source dimensions the last
    // row/column belongs to no 2x2 block — a plain 2x2 reduce silently DROPS
    // that depth from the pyramid, and everything behind it gets falsely
    // occlusion-culled (view-dependent black meshes: canopy posts, far roofs).
    // Fold the odd remainder into the edge texels' blocks (3-wide/3-tall).
    ivec2 base  = dstCoord * 2;
    ivec2 srcMax = pc.srcSize - 1;
    vec2  texelSize = 1.0 / vec2(pc.srcSize);

    float maxDepth = 0.0;
    const bool oddX = (pc.srcSize.x & 1) != 0 && dstCoord.x == pc.dstSize.x - 1;
    const bool oddY = (pc.srcSize.y & 1) != 0 && dstCoord.y == pc.dstSize.y - 1;
    const int nx = oddX ? 3 : 2;
    const int ny = oddY ? 3 : 2;
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            ivec2 c = min(base + ivec2(x, y), srcMax);
            float d = textureLod(uSrcDepth, (vec2(c) + 0.5) * texelSize, float(pc.srcMip)).r;
            maxDepth = max(maxDepth, d);   // MAX = conservative (cull only behind the farthest)
        }

    imageStore(uDstMip, dstCoord, vec4(maxDepth, 0.0, 0.0, 0.0));
}
