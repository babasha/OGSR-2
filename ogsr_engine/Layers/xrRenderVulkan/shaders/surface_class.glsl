// xrRenderVulkan - Surface classification for the Surface Field. The renderer
// already knows what a fragment IS by which shader draws it (world_terrain =
// ground, tree = foliage, detail = grass, world_lmap/vlit = building/prop static);
// the world NORMAL then splits a static into roof/floor (up), wall (vertical) or
// ceiling (down). Per-pixel, no map needed. Consumers (snow, water, wetness) read
// the refined type so each surface behaves correctly (snow piles on roofs+ground,
// none on walls, light frost on foliage, ...). #include needs only the normal.
#ifndef SURFACE_CLASS_GLSL
#define SURFACE_CLASS_GLSL

// BASE class each shader passes in (it knows what it draws).
const int SC_GROUND  = 0;   // terrain (world_terrain)
const int SC_STATIC  = 1;   // building / prop (world_lmap / world_vlit) - refined by normal
const int SC_FOLIAGE = 2;   // tree / bush canopy (tree shader)
const int SC_GRASS   = 3;   // grass (detail shader)

// REFINED type: 0 ground, 1 roof/floor (up static), 2 wall (vertical static),
// 3 ceiling (down static), 4 foliage, 5 grass.
int SC_Refine(int base, vec3 N) {
    if (base == SC_GROUND)  return 0;
    if (base == SC_FOLIAGE) return 4;
    if (base == SC_GRASS)   return 5;
    if (N.y >  0.5) return 1;   // up-facing static  -> roof / floor
    if (N.y < -0.3) return 3;   // down-facing static -> ceiling / underside
    return 2;                   // vertical static    -> wall
}

// Debug heatmap colour per refined type.
vec3 SC_DebugColor(int t) {
    if (t == 0) return vec3(0.20, 0.80, 0.25);   // ground   - green
    if (t == 1) return vec3(0.25, 0.50, 1.00);   // roof     - blue
    if (t == 2) return vec3(1.00, 0.25, 0.20);   // wall     - red
    if (t == 3) return vec3(1.00, 0.55, 0.10);   // ceiling  - orange
    if (t == 4) return vec3(0.90, 0.85, 0.20);   // foliage  - yellow
    return vec3(0.20, 0.85, 0.90);               // grass    - cyan
}

// Snow accumulation 0..1 by type + slope (from N) + sky exposure. The first real
// consumer of the classification (preview/whitening lands in the snow step). Roof
// and ground pile snow by flatness; walls/ceilings none; foliage light frost;
// grass covered. `exposure` = SF_SkyExposure (open to the sky).
float SC_SnowAmount(int t, vec3 N, float exposure) {
    float e    = clamp(exposure, 0.0, 1.0);
    float hold = smoothstep(0.35, 0.80, clamp(N.y, 0.0, 1.0));   // steep -> no hold
    if (t == 0 || t == 1) return hold * e;          // ground / roof
    if (t == 5)           return hold * e * 0.85;   // grass (covers, bends)
    if (t == 4)           return 0.40 * e;          // foliage: light frosting
    return 0.0;                                     // wall / ceiling: none
}

#endif // SURFACE_CLASS_GLSL
