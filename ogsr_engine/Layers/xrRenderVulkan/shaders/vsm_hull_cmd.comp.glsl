#version 450
// xrRenderVulkan — VSM crown-HULL/VOXEL indirect builder (r_vsm_tree_hull). One thread
// per tree, handles BOTH tiers in one dispatch (recorded at the end of VsmBinDyn, after
// both stage-1 bins wrote their indirect buffers):
//   • DYN tier  (nearFlags bit 1): near trees beyond r_vsm_tree_hull_dist — swap the
//     crown cmd in SrcDyn for the proxy, zero the crown cmd.
//   • STATIC tier (nearFlags bit 2, r_vsm_tree_hull 2): FAR foliage whose shadow column
//     is beyond the same distance — same swap on the STATIC bin's buffer.
// The PROXY is either the merged shell (indexed hull cmd — legacy, r_vsm_tree_hull_vox
// 0) or the VOXEL CLOUD (voxOn): a NON-indexed VkDrawIndirectCommand with vertexCount =
// 6·voxCount of the tree's camera-distance LOD (choice buffer, CPU-computed) — each
// voxel casts as ONE light-facing quad, not a 36-vert cube (VS cost, see the VS body).
// CROSSFADE BAND (voxOn, fade16 in choice.w < 0xFFFF): trees approaching the boundary
// draw BOTH — the crown cmd is NOT zeroed and the voxel FS dissolves the cubes in by
// screen-door — so the shadow smoothly morphs to true leaves as the camera closes in.
// instanceCount + firstInstance are copied verbatim → the proxy VS reuses the exact
// casterPages page routing. Zeroing the crown cmd also disables the meshlet stage-2
// refinement for the tree. Commands for non-proxy trees are always written with
// instanceCount 0 — stale counts must not draw.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std430) buffer SrcDyn { uint srcD[]; };                   // dyn crown VkDrawIndexedIndirectCommand[count] (RW)
layout(set = 0, binding = 1, std430) readonly buffer HullInfo { uvec4 hullInfo[]; };   // per tree: x=ib_first y=ib_count z=vb_first (0 = no hull baked)
layout(set = 0, binding = 2, std430) readonly buffer NearFlags { uint nearFlags[]; };  // bit0=near bit1=near-hull bit2=far-hull
layout(set = 0, binding = 3, std430) writeonly buffer DstDyn { uint dstD[]; };         // dyn hull cmds (indexed, 5 u32)
layout(set = 0, binding = 4, std430) buffer SrcStat { uint srcS[]; };                  // STATIC crown cmds (RW)
layout(set = 0, binding = 5, std430) writeonly buffer DstStat { uint dstS[]; };        // static hull cmds
layout(set = 0, binding = 6, std430) readonly buffer Choice { uvec4 choice[]; };       // per tree: brickFirst, brickCount, cellBits, fade16<<16|amp16
layout(set = 0, binding = 7, std430) writeonly buffer DstVoxDyn { uint dvD[]; };       // dyn voxel cmds (NON-indexed, 4 u32)
layout(set = 0, binding = 8, std430) writeonly buffer DstVoxStat { uint dvS[]; };      // static voxel cmds

layout(push_constant) uniform PC { uint count; uint farOn; uint voxOn; } pc;

void main()
{
    uint t = gl_GlobalInvocationID.x;
    if (t >= pc.count) return;
    uint  s = t * 5u;   // VkDrawIndexedIndirectCommand = 5 u32
    uint  q = t * 4u;   // VkDrawIndirectCommand = 4 u32
    uvec4 h = hullInfo[t];
    uint  f = nearFlags[t];

    uvec4 c      = choice[t];
    uint  fade16 = c.w >> 16;
    bool  voxAny = (pc.voxOn & 1u) != 0u && c.y > 0u && fade16 > 0u;
    // Crossfade: the crown joins only the INNER half of the band (fade < 0.5). The
    // outer half is the near-range sparsification stage — cubes alone, thinning in
    // leaf-coverage order (VS) so the shadow reads as cubes at leaf positions; by the
    // time the real crown lights up, the survivors are a sparse subset of its shadow.
    bool  band   = voxAny && fade16 < 0x8000u;

    // ---- DYN tier: voxels take any near-set tree with a live fade (band or full);
    //      the legacy shell only when voxels are unavailable for a bit1 tree.
    bool voxD  = voxAny && (f & 3u) != 0u;
    bool hullD = !voxD && (f & 2u) != 0u && h.y > 0u;
    // Band prefix cut: slices are bake-sorted by descending dissolve rank, so the
    // survivors of fade f are its first f·count bricks — draw only those (+2 for the
    // soft-window edge) instead of every brick with a VS kill.
    uint effD = c.y;
    if (fade16 < 0xFFFFu) effD = min(c.y, (c.y * fade16) / 65535u + 2u);
    dvD[q + 0u] = 6u * effD;
    dvD[q + 1u] = voxD ? srcD[s + 1u] : 0u;
    dvD[q + 2u] = 6u * c.x;
    dvD[q + 3u] = srcD[s + 4u];
    dstD[s + 0u] = h.y;
    dstD[s + 1u] = hullD ? srcD[s + 1u] : 0u;
    dstD[s + 2u] = h.x;
    dstD[s + 3u] = h.z;
    dstD[s + 4u] = srcD[s + 4u];
    // Zero the crown ONLY for fully-proxied trees (bit1 past the band); band trees keep
    // casting the real crown while the voxels dissolve in.
    if ((hullD || voxD) && (f & 2u) != 0u && !band) srcD[s + 1u] = 0u;

    // ---- STATIC tier (mode 2): far foliage, always fade 1 (no band this far out).
    // Bricks only when voxOn bit1 (r_vsm_tree_hull_vox_static) — default is the merged
    // SHELL: blind-test-verified at these distances, and it doesn't pay the missing
    // per-page brick cull on every dirty cache page.
    bool voxS  = pc.farOn != 0u && voxAny && (pc.voxOn & 2u) != 0u && (f & 4u) != 0u;
    bool hullS = !voxS && pc.farOn != 0u && (f & 4u) != 0u && h.y > 0u;
    dvS[q + 0u] = 6u * c.y;
    dvS[q + 1u] = voxS ? srcS[s + 1u] : 0u;
    dvS[q + 2u] = 6u * c.x;
    dvS[q + 3u] = srcS[s + 4u];
    dstS[s + 0u] = h.y;
    dstS[s + 1u] = hullS ? srcS[s + 1u] : 0u;
    dstS[s + 2u] = h.x;
    dstS[s + 3u] = h.z;
    dstS[s + 4u] = srcS[s + 4u];
    if (hullS || voxS) srcS[s + 1u] = 0u;
}
