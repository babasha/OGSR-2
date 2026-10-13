#version 450
// TERRAIN COMPOSITE CACHE, cone bake pass 2/2 (r_terra_cone): for every cache
// texel compute the widest EMPTY CONE anchored at its surface point - ratio =
// min over higher texels q of dist(p,q) / (h(q) - h(p)), i.e. "how far the POM
// ray may travel per unit of clearance above the surface with a guaranteed
// no-hit". Written into .g of the RG16F height cache, so the fragment march
// gets height+cone in ONE tap and leaps 5-8 cone steps instead of walking 24
// fixed layers (no stair-stepping, no missed thin crack walls at grazing).
//
// Search = exact 3x3 ring at full res + 5x5-minus-centre block rings on the
// max-pyramid at block sizes 2/4/8 (levels 0..2). Coverage: a texel at
// Chebyshev distance D lands in the B<=D<=2B level's +/-2 block window, so
// everything within 16 texels is bounded. Beyond 16 texels is uncovered BY
// DESIGN: the cone is clamped to kConeMaxTexels = 32, and a march step covers
// at most cone * POM_PLANE(0.5) = 16 texels - never past the searched radius.
// Block distances use centre - half-diagonal (conservative), floored at B/2
// (nearer texels are bounded more precisely by the finer rings).
// HORIZON MAP (r_terra_horizon): the same pass also bakes the directional
// HORIZON toward the CURRENT SUN AZIMUTH into .b - max slope (dh / distance,
// cache-UV units) over ~14 samples marching sun-ward (near = exact texels,
// far = pyramid blocks, out to 32 texels / ~75 cm; macro shadows are VSM's
// job). The fragment shader then does a geometric tan(sun) vs tan(horizon)
// test instead of an 8-tap height march. One direction only: the sun is one
// vector per frame, and vk_terrain_cache re-bakes when the azimuth drifts.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 0, rgba16f) uniform image2D imgHeight;  // .r read (heights); .rgba written (h, cone, horizon, 0)
layout(set = 1, binding = 2) uniform sampler2D uPyr;              // height max-pyramid (1024/512/256)

layout(push_constant) uniform Push {
    vec4 sun;   // xy = normalized cache-space direction TOWARD the sun; z = horizon on/off
} pc;

const float kConeMaxTexels = 32.0;
const float kHorMaxSlope   = 4096.0;   // dh per cache-UV (~1 height-unit over 0.5 texel)

void main()
{
    ivec2 res = imageSize(imgHeight);
    ivec2 px  = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= res.x || px.y >= res.y) return;

    float h0 = imageLoad(imgHeight, px).r;
    float c  = kConeMaxTexels;             // cone ratio, full-res texels per height-unit

    // Exact 3x3 ring (-0.5 texel: the march samples the field bilinearly, a
    // high texel already raises the surface half a texel before its centre).
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0) continue;
        ivec2 q  = clamp(px + ivec2(dx, dy), ivec2(0), res - 1);
        float dh = imageLoad(imgHeight, q).r - h0;
        if (dh > 1e-4)
            c = min(c, max(length(vec2(dx, dy)) - 0.5, 0.35) / dh);
    }

    // Pyramid block rings.
    vec2 p = vec2(px) + 0.5;
    for (int l = 0; l < 3; ++l) {
        float B    = float(2 << l);            // block size in full-res texels
        ivec2 pres = res >> (l + 1);           // pyramid level l resolution
        ivec2 pb   = px >> (l + 1);            // own block
        for (int by = -2; by <= 2; ++by)
        for (int bx = -2; bx <= 2; ++bx) {
            if (bx == 0 && by == 0) continue;
            ivec2 b = pb + ivec2(bx, by);
            if (any(lessThan(b, ivec2(0))) || any(greaterThanEqual(b, pres))) continue;
            float dh = texelFetch(uPyr, b, l).r - h0;
            if (dh <= 1e-4) continue;
            vec2  bc = (vec2(b) + 0.5) * B;
            float d  = max(length(bc - p) - 0.7071 * B - 0.5, 0.5 * B);
            c = min(c, d / dh);
        }
    }

    // Sun horizon: max slope toward the sun. Near samples exact (imageLoad),
    // far ones from the max-pyramid (block max -> slightly conservative, i.e.
    // shadows err a touch wider; the shading smoothstep softens the edge).
    float hor = 0.0;
    if (pc.sun.z > 0.5) {
        const float kD[14] = float[14](1.0, 1.5, 2.0, 2.5, 3.25, 4.0,      // exact
                                       5.0, 6.5, 8.0,                      // mip 0 (blocks of 2)
                                       10.5, 13.0, 16.0,                   // mip 1 (blocks of 4)
                                       21.0, 28.0);                        // mip 2 (blocks of 8)
        for (int i = 0; i < 14; ++i) {
            float d = kD[i];
            vec2  sp = p + pc.sun.xy * d;
            float hq;
            if (i < 6) {
                ivec2 q = clamp(ivec2(sp), ivec2(0), res - 1);
                hq = imageLoad(imgHeight, q).r;
            } else {
                int   ml = (i < 9) ? 0 : (i < 12) ? 1 : 2;
                ivec2 q  = clamp(ivec2(sp) >> (ml + 1), ivec2(0), (res >> (ml + 1)) - 1);
                hq = texelFetch(uPyr, q, ml).r;
            }
            hor = max(hor, (hq - h0) / d);
        }
        hor = min(hor * float(res.x), kHorMaxSlope);   // texel slope -> cache-UV slope
    }

    // Store in cache-UV units (the march works in UV space).
    imageStore(imgHeight, px, vec4(h0, c / float(res.x), hor, 0.0));
}
