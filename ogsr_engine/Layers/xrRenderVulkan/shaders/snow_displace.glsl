// xrRenderVulkan - shared SNOW geometric displacement. Raises a surface vertex
// along its normal by the local snow depth so snow has real VOLUME (silhouette
// rise), not just a flat albedo tint. MUST be applied IDENTICALLY in every pass
// that rasterizes the surface (color + depth prepass + shadows) or early-Z will
// z-fight. It is a PURE function of (worldPos, N, coverage) so any stage that knows
// those produces the same result. coverage = L.sf_params.w (accumulated snow).
//
// NOTE: this matches the FLATNESS term of SC_SnowAmount (ground/roof) but omits sky
// exposure (the depth-only passes have no rain map) - so under solid cover the
// geometry rises slightly without the albedo turning white. Kept small + a later
// pass can gate it. Tune SNOW_MAX_DEPTH.
#ifndef SNOW_DISPLACE_GLSL
#define SNOW_DISPLACE_GLSL

const float SNOW_MAX_DEPTH = 0.35;   // metres of snow on flat open ground at full coverage

// Cheap drift noise over world XZ (uneven piling), ~0.55..1.0.
float snowDrift(vec2 xz) {
    float n = sin(xz.x * 0.7 + 1.3) * cos(xz.y * 0.6 - 0.7)
            + 0.5 * sin(xz.x * 1.9 - 2.1) * cos(xz.y * 1.7 + 0.4);
    return 0.55 + 0.45 * (n * 0.5 + 0.5);
}

// Snow depth (metres) at a world position with geometric normal N (up = +Y).
// Flat up-facing accumulates; steep/under-facing none. coverage 0..1 (global).
// NOTE: footprints are NOT carved here - the terrain mesh is too COARSE (~1-2 m
// verts) to represent a ~20 cm print via vertex displacement (sub-vertex). Prints
// are done in the FRAGMENT (world_terrain.frag SnowFootprint) as a shaded dimple.
float SnowDepthAt(vec3 worldPos, vec3 N, float coverage) {
    if (coverage <= 0.0) return 0.0;
    float flatness = smoothstep(0.35, 0.80, clamp(N.y, 0.0, 1.0));   // matches SC_SnowAmount ground/roof
    return SNOW_MAX_DEPTH * coverage * flatness * snowDrift(worldPos.xz);
}

// ---- Texture-based deform (r_snow_deform_tex): a dense persistent press field
// (vk_deform) sampled in a player-centred ortho box. O(1), smooth, persistent for
// the whole box (~a minute) - the scalable replacement for the stamp loop. The base
// terrain mesh is too coarse for tessellation (cap 64) to carve footprint-scale
// geometry without faceting, so the GEOMETRY samples a BROAD blur (low-freq trodden
// groove the coarse tess CAN show smoothly) while the FRAGMENT samples the SHARP field
// for the per-pixel round dent. L.deform_tex: x=enable, y=1/size, z=max depth, w=m/texel.
bool SnowDeformOn() { return L.deform_tex.x > 0.5; }

float SnowDeformPress(vec3 wp) {
    // TOROIDAL world-anchored field: uv = worldXZ / kWorld, the sampler is REPEAT so the
    // 2048² tile wraps and a print keeps a FIXED world position (no per-frame reproject).
    // Only the ±kHalf window around the eye is valid — beyond it a texel aliases a world
    // point kWorld away that this frame's window doesn't own, so gate on eye distance.
    // kHalf = 0.5*(m/texel)*size, 1/kWorld = (1/size)/(m/texel), both from deform_tex (.w,.y).
    vec2  rel     = wp.xz - L.eye_pos.xz;
    float winHalf = 0.5 * L.deform_tex.w / L.deform_tex.y;
    if (max(abs(rel.x), abs(rel.y)) >= winHalf) return 0.0;
    float invW = L.deform_tex.y / L.deform_tex.w;
    return textureLod(uDeform, wp.xz * invW + 0.5, 0.0).r;
}

// Broad low-freq press for GEOMETRY: footprint-scale detail removed (taps ~0.55 m
// apart) so the coarse tess shows a smooth trodden depression, not facets.
float SnowDeformPressBroad(vec3 wp) {
    const float o = 0.55;
    float p = SnowDeformPress(wp)
            + SnowDeformPress(wp + vec3( o, 0.0, 0.0))
            + SnowDeformPress(wp + vec3(-o, 0.0, 0.0))
            + SnowDeformPress(wp + vec3(0.0, 0.0,  o))
            + SnowDeformPress(wp + vec3(0.0, 0.0, -o));
    return p * 0.2;
}

// GEOMETRIC footprint carve (metres): a smooth ROUND basin inside each recent print +
// a low ridge of displaced snow around it. For the terrain tessellation evaluation
// shader. Coarse tessellation can only show a wide smooth bowl without faceting, so
// the profile is a cosine bell (no hard rim) - the crisp print edge is added per-pixel
// in SnowFootprint instead. A cheap per-stamp AABB reject keeps a long persistent
// trail at ~O(nearby prints). Negative = pressed down.
float SnowFootprintCarve(vec2 xz) {
    if (SnowDeformOn())
        return -SnowDeformPressBroad(vec3(xz.x, 0.0, xz.y)) * L.deform_tex.z;   // broad smooth groove
    if (L.deform_count.w <= 0.5) return 0.0;
    int   dn     = int(L.deform_count.x + 0.5);
    float press  = L.deform_count.y;
    float ridgeH = L.deform_count.z;
    float delta  = 0.0;
    for (int i = 0; i < dn; ++i) {
        vec4  st  = L.deform_stamps[i];
        if (st.w <= 0.0) continue;
        // GEOMETRY uses a BROAD bowl (3x the print radius): a foot dent must span several
        // tessellation triangles, else the coarse terrain mesh facets it into "big
        // triangles". The crisp per-foot print is added per-pixel in SnowFootprint, so
        // the broad smooth groove + sharp pixel print read together as a trodden trail.
        float rad   = max(st.z, 0.05) * 3.0;
        float reach = rad * 1.7;
        if (abs(xz.x - st.x) > reach || abs(xz.y - st.y) > reach) continue;   // AABB reject
        float nd  = distance(xz, st.xy) / rad;                                // 0 centre .. 1 rim
        float bowl = (nd < 1.0) ? (0.5 + 0.5 * cos(3.14159265 * nd)) : 0.0;   // smooth round basin
        delta -= press * st.w * bowl;
        float ring = smoothstep(1.0, 1.25, nd) * (1.0 - smoothstep(1.25, 1.7, nd));   // displaced rim
        delta += ridgeH * st.w * ring;
    }
    return delta;
}

// FRAGMENT footprint: at recent foot contacts press the snow look into a packed,
// dimpled depression (analytic = perfectly ROUND at any mesh resolution, unlike the
// tessellation-limited vertex carve). Darkens albedo toward packed snow and tilts the
// normal outward so the dip catches light - this is what makes the print read as round
// even where the geometry silhouette is coarse. `snow` = local snow amount (no prints
// where there's no snow). Call after the snow whitening in the fragment.
void SnowFootprint(inout vec3 albedo, inout vec3 Nw, vec3 worldPos, float snow) {
    if (snow <= 0.01) return;

    // Texture path: sample the SHARP press field + its gradient for a smooth round,
    // per-pixel dent (no tessellation faceting, persistent for the whole box).
    if (SnowDeformOn()) {
        if (distance(worldPos, L.eye_pos.xyz) > 60.0) return;
        float p = SnowDeformPress(worldPos);
        if (p <= 0.002) return;
        float ew   = L.deform_tex.w;            // world metres per texel
        float maxD = L.deform_tex.z;            // dent depth (m)
        float pX = SnowDeformPress(worldPos + vec3(ew, 0.0, 0.0));
        float pXm= SnowDeformPress(worldPos - vec3(ew, 0.0, 0.0));
        float pZ = SnowDeformPress(worldPos + vec3(0.0, 0.0, ew));
        float pZm= SnowDeformPress(worldPos - vec3(0.0, 0.0, ew));
        // surface height h = -press*maxD (down); normal = (-dh/dx, 1, -dh/dz).
        vec2 slope = vec2(pX - pXm, pZ - pZm) * (maxD / (2.0 * ew));
        vec3 dN = normalize(vec3(slope.x, 1.0, slope.y));
        float t = p * snow;
        albedo = mix(albedo, albedo * vec3(0.58, 0.62, 0.70), t);            // packed, darker, cooler
        Nw = normalize(mix(Nw, dN, clamp(t * 1.2, 0.0, 0.9)));               // smooth round dent
        return;
    }

    if (L.deform_count.w <= 0.5) return;
    if (distance(worldPos, L.eye_pos.xyz) > 50.0) return;   // sub-decimetre detail: near only (gates the loop)
    int dn = int(L.deform_count.x + 0.5);
    float pr = 0.0;
    vec2  outward = vec2(0.0);
    for (int i = 0; i < dn; ++i) {
        vec4  st  = L.deform_stamps[i];
        if (st.w <= 0.0) continue;
        float rad = max(st.z, 0.05);
        if (abs(worldPos.x - st.x) > rad || abs(worldPos.z - st.y) > rad) continue;   // AABB reject
        float d   = distance(worldPos.xz, st.xy);
        if (d < rad) {
            float t = (1.0 - smoothstep(rad * 0.35, rad, d)) * st.w;   // 1 centre .. 0 rim
            pr = max(pr, t);
            outward += normalize(worldPos.xz - st.xy + 1e-4) * t;      // dip wall faces outward
        }
    }
    pr *= snow;
    if (pr <= 0.0) return;
    albedo = mix(albedo, albedo * vec3(0.58, 0.62, 0.70), pr);                 // packed, darker, cooler
    Nw = normalize(Nw + vec3(outward.x, 0.0, outward.y) * (pr * 1.3));         // stronger round dimple
}

// Displace a world-space vertex up its normal by the local snow depth.
vec3 SnowDisplace(vec3 worldPos, vec3 N, float coverage) {
    return worldPos + N * SnowDepthAt(worldPos, N, coverage);
}

#endif // SNOW_DISPLACE_GLSL
