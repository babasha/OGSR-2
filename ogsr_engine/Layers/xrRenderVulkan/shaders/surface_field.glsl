// xrRenderVulkan - Surface Field: a queryable representation of the ground surface
// (height / slope / curvature / sky-exposure / canopy) that procedural systems
// (snow, leaf litter, valley fog, water/puddle bias) READ and deformation will
// WRITE. The "smart heightmap" substrate.
//
// PHASE 2.0: derived IN-SHADER. HEIGHT/CURVATURE come from the top-down RAIN
// occlusion map (uRainMap, b9) at metre-scale (~14.6 cm/texel over +-75 m) - the
// known-good, actually-rendered map (statics+terrain; +trees only WHILE RAINING).
// We use the rain map (matching groundHm), NOT the dedicated no-trees ground map
// (uGround, b13), which is only rendered when r_water_sim is on. SLOPE comes from
// the shaded fragment's OWN normal (clean per-pixel; the map's canopy/roof tops
// would otherwise read as huge false slopes). A later phase bakes a dedicated near
// cascade (own no-overhead render, toroidal) + the derivatives into a compute
// texture, fixing the top-down map's overhead-structure noise at the source; the
// SF_* API stays the same. #include AFTER light_ubo.glsl (uRainMap b9, rain_vp,
// sf_params) and env_common.glsl (rainVis = sky exposure).
#ifndef SURFACE_FIELD_GLSL
#define SURFACE_FIELD_GLSL

// Rain ortho depth range (vk_shadow: kRainZFar - kRainZNear = 350 - 1). Matches groundHm().
const float SF_ZRANGE = 349.0;

// World -> rain-map UV (straight-down ortho). Returns false if off-map.
bool SF_MapUV(vec3 wp, out vec2 uv) {
    vec4 c = L.rain_vp * vec4(wp, 1.0);
    if (c.w <= 0.0) { uv = vec2(0.0); return false; }
    uv = c.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    return !(any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))));
}

// Surface height (metres, relative; higher = larger) at wp's XZ from the rain map.
float SF_HeightAt(vec3 wp) {
    vec2 uv; if (!SF_MapUV(wp, uv)) return 0.0;
    return -textureLod(uRainMap, uv, 0.0).r * SF_ZRANGE;
}

// Finite-difference epsilon (metres) for the map derivatives (sf_params.z = r_sf_eps).
float SF_Eps() { return max(L.sf_params.z, 0.05); }

// Sky openness: 1 = open (rained/snowed on, lit), 0 = covered (roof/tunnel/canopy).
float SF_SkyExposure(vec3 wp) { return rainVis(wp); }

// SLOPE from the shaded surface's OWN geometric normal: 0 = flat (up-facing),
// 1 = vertical. Per-pixel and CLEAN - independent of the top-down map, so tree
// canopy / roof tops in the map don't create false slopes. This is what snow /
// water sitting on the fragment actually see. (Pass the geometric normal.)
float SF_SlopeFromN(vec3 N) { return clamp(length(N.xz), 0.0, 1.0); }

// Is this fragment a valid OPEN-GROUND sample for the top-down field? Up-facing
// AND the topmost surface at its XZ. Rejects walls/fences (vertical), ceilings
// (down-facing), and interior floors / ground-under-canopy (where the map's depth
// describes a roof/canopy ABOVE the fragment, not the fragment itself). The
// height/curvature debug and future consumers (snow/water) gate by this.
float SF_Valid(vec3 wp, vec3 N) {
    float up = smoothstep(0.35, 0.60, N.y);
    if (up <= 0.0) return 0.0;
    vec2 uv; if (!SF_MapUV(wp, uv)) return up;                     // off-map -> treat as open
    float fragZ = (L.rain_vp * vec4(wp, 1.0)).z;                  // fragment depth in the rain ortho
    float mapZ  = textureLod(uRainMap, uv, 0.0).r;                // stored topmost-surface depth
    float onTop = 1.0 - smoothstep(0.0015, 0.005, fragZ - mapZ);  // fragment below the top surface -> invalid
    return up * onTop;
}

// Mean curvature (Laplacian of height) from the map. >0 concave (basins/ruts/pits
// where rain+snow+leaves collect), <0 convex (ridges/bumps/edges). Per-tap height
// deltas are CLIFF-CLAMPED so vertical structure edges (walls, roof rims) don't
// blow the value up into blocky garbage.
float SF_Curvature(vec3 wp) {
    float e = SF_Eps();
    float maxD = e * 1.5;   // reject deltas steeper than ~tan 56deg (walls/cliffs/canopy rims)
    float h   = SF_HeightAt(wp);
    float dx1 = clamp(SF_HeightAt(wp + vec3( e, 0.0, 0.0)) - h, -maxD, maxD);
    float dx0 = clamp(SF_HeightAt(wp + vec3(-e, 0.0, 0.0)) - h, -maxD, maxD);
    float dz1 = clamp(SF_HeightAt(wp + vec3(0.0, 0.0,  e)) - h, -maxD, maxD);
    float dz0 = clamp(SF_HeightAt(wp + vec3(0.0, 0.0, -e)) - h, -maxD, maxD);
    return (dx1 + dx0 + dz1 + dz0) / (e * e);
}

// Concavity 0..1: open basins where rain/snow/leaves/fog collect. Positive
// curvature = concave, GATED by sky exposure so (a) the unreliable under-cover map
// reads neutral and (b) it only fires where the sky is actually open (physically:
// nothing collects under a solid roof). The primary consumer signal.
float SF_Concavity(vec3 wp) { return clamp(SF_Curvature(wp) * 4.0, 0.0, 1.0) * SF_SkyExposure(wp); }

// Canopy/overhead mass. DEFERRED to the 2.1 cascade (needs a with-trees minus
// no-trees diff; the no-trees map isn't rendered here). Sky exposure (mode 4)
// already captures "covered overhead" meanwhile.
float SF_Canopy(vec3 wp) { return 0.0; }

// Debug visualization (r_sf_debug): 1 height (5 m bands), 2 slope (from normal),
// 3 curvature (blue basin / red ridge, exposure-gated), 4 sky exposure, 5 canopy.
vec3 SF_DebugColor(vec3 wp, vec3 N, int mode) {
    if (mode == 2) return vec3(SF_SlopeFromN(N));               // clean per-pixel (ungated)
    if (mode == 4) return vec3(SF_SkyExposure(wp));
    if (mode == 5) return vec3(SF_Canopy(wp));
    float valid = SF_Valid(wp, N);                             // only open up-facing top ground
    if (mode == 1) return vec3(fract(SF_HeightAt(wp) * 0.2) * valid);   // 1 band / 5 m
    // mode 3: curvature, blue basin / red ridge, off non-ground surfaces. The
    // extra blurred sky-exposure term softly suppresses the tree/canopy vicinity
    // (the rain map's blocky canopy depth makes the sharp `valid` test checkerboard
    // there) instead of leaving hard squares.
    float c     = SF_Curvature(wp) * valid * SF_SkyExposure(wp);
    float pit   = clamp( c * 4.0, 0.0, 1.0);                   // concave basin -> blue
    float ridge = clamp(-c * 4.0, 0.0, 1.0);                   // convex ridge  -> red
    return vec3(0.08 * valid + ridge, 0.08 * valid, 0.08 * valid + pit);
}

#endif // SURFACE_FIELD_GLSL
