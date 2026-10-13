#version 450
// xrRenderVulkan — crown voxel-BRICK caster FS: DDA raycast of the brick's 64-bit
// occupancy mask (UE Nanite RasterizeBricks model, simplified for our case). The VS
// rasterizes ONE quad over the brick's light-space AABB; here each covered fragment
// reconstructs its entry point in brick-cell coords ([0,4]³) and marches the ray along
// the sun (constant per brick — the VSM view is orthographic, so the whole march setup
// arrives precomputed in flat interpolants) until it hits an occupied cell, then writes
// that cell's conservative light depth. Empty columns discard — the shadow shows the
// true voxel silhouette with gaps, not the AABB slab. Worst case ~10 cell steps; the
// depth_greater re-declaration keeps early-Z alive (the quad rasterizes at the brick's
// MIN depth and the march can only deepen it).
//
// CROSSFADE (vFade < 1, the band below r_vsm_tree_hull_dist where the real alpha-tested
// crown still casts): whole-brick screen-door dissolve by IGN noise, before the march.
layout(location = 0) in flat float vFade;
layout(location = 1) in flat uvec2 vMask;    // 64-bit occupancy (full or core — VS picks)
layout(location = 2) in flat vec3  vMx;      // ∂brick/∂lp.x (cell units)
layout(location = 3) in flat vec3  vMy;      // ∂brick/∂lp.y
layout(location = 4) in flat vec3  vDir;     // ∂brick/∂lp.z = ray dir per light-z meter
layout(location = 5) in flat vec3  vB0;      // brick coords at (vLpRef, brick minLz)
layout(location = 6) in flat vec2  vLpRef;   // light-space reference point (brick origin xy)
layout(location = 7) in noperspective vec2 vLpXY;   // this fragment's light-space xy
layout(location = 8) in flat vec2  vNz;      // x = normalized depth at minLz, y = depth per light-z meter

layout(depth_greater) out float gl_FragDepth;

// Cell (x,y,z) ∈ [0,3]³ → mask bit x | y<<2 | z<<4 (matches the bake).
bool occ(ivec3 c)
{
    uint bit = uint(c.x) | (uint(c.y) << 2) | (uint(c.z) << 4);
    return ((bit < 32u ? vMask.x >> bit : vMask.y >> (bit - 32u)) & 1u) != 0u;
}

void main()
{
    if (vFade < 0.999) {
        float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        if (ign >= vFade) discard;
    }

    // Ray in brick-cell coords: origin at this fragment's xy on the brick's minLz
    // plane, direction = cells per light-z meter (t below is in light-z meters).
    vec2 d   = vLpXY - vLpRef;
    vec3 p0  = vB0 + vMx * d.x + vMy * d.y;
    // Zero components stall the DDA (inf tMax never advances that axis) — nudge them.
    vec3 dir = mix(vDir, vec3(1e-5), lessThan(abs(vDir), vec3(1e-5)));
    vec3 idir = 1.0 / dir;

    // Slab-clip into the [0,4]³ box (the quad is the box's xy AABB — corners overhang).
    vec3  t0 = (vec3(0.0) - p0) * idir;
    vec3  t1 = (vec3(4.0) - p0) * idir;
    vec3  tn = min(t0, t1), tf = max(t0, t1);
    float tEnter = max(max(tn.x, tn.y), max(tn.z, 0.0));
    float tExit  = min(tf.x, min(tf.y, tf.z));
    if (tEnter > tExit) discard;

    // Amanatides–Woo DDA. 4³ grid → ≤ 10 visited cells on any straight ray.
    vec3  p     = p0 + dir * tEnter;
    ivec3 c     = clamp(ivec3(floor(p)), ivec3(0), ivec3(3));
    ivec3 stepC = ivec3(sign(dir));
    vec3  tMax  = (vec3(c) + max(sign(dir), vec3(0.0)) - p0) * idir;
    vec3  tDel  = abs(idir);
    float t     = tEnter;

    for (int i = 0; i < 12; ++i) {
        if (occ(c)) {
            gl_FragDepth = vNz.x + max(t, 0.0) * vNz.y;
            return;
        }
        if (tMax.x < tMax.y) {
            if (tMax.x < tMax.z) { c.x += stepC.x; t = tMax.x; tMax.x += tDel.x; if (uint(c.x) > 3u) break; }
            else                 { c.z += stepC.z; t = tMax.z; tMax.z += tDel.z; if (uint(c.z) > 3u) break; }
        } else {
            if (tMax.y < tMax.z) { c.y += stepC.y; t = tMax.y; tMax.y += tDel.y; if (uint(c.y) > 3u) break; }
            else                 { c.z += stepC.z; t = tMax.z; tMax.z += tDel.z; if (uint(c.z) > 3u) break; }
        }
    }
    discard;
}
