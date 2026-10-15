#version 450
#extension GL_GOOGLE_include_directive : require
#define ENV_SET 0
#include "light_ubo.glsl"      // L + set0 lighting samplers
#include "shadow_common.glsl"  // sunShadow
#include "env_common.glsl"     // skyAmbient, rainVis
#include "snow_displace.glsl"  // SnowDeformPress (dent albedo)

// SNOW MESH fragment: shade the dense snow surface. The dent SHAPE comes from the
// vertex displacement (real geometry -> smooth, no faceting) so here we only darken
// the albedo inside dents (packed snow) and light it like the terrain snow (sun +
// sky ambient + ambient + fog). Discards where there is no snow (steep / under cover
// / no coverage / past the rim) so the bare terrain underneath shows through.

layout(push_constant) uniform PC {
    mat4 mvp; vec4 p0; vec4 p1; vec4 p2;   // p1.x=dent depth, p1.w=debug; p2.x=ripple strength
} pc;

// Cheap value noise for snow grain / sparkle (no texture).
float sHash(vec2 p) { p = fract(p * vec2(127.1, 311.7)); p += dot(p, p + 34.23); return fract(p.x * p.y); }
float snowGrain(vec2 p) {
    vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
    float a = sHash(i), b = sHash(i + vec2(1,0)), c = sHash(i + vec2(0,1)), d = sHash(i + vec2(1,1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// Rotated multi-octave noise: each octave is rotated + non-integer scaled so NO
// axis-aligned lattice survives -> avoids the "tyre-tread" grid that a single
// grid-aligned octave makes when viewed at a grazing angle. Returns ~0..1.
float fbmGrain(vec2 p) {
    const mat2 R = mat2(0.80, -0.60, 0.60, 0.80);   // ~37 deg
    float s = 0.0, a = 0.55, w = 0.0;
    for (int i = 0; i < 3; ++i) { s += a * snowGrain(p); w += a; p = R * (p * 1.93); a *= 0.5; }
    return s / w;
}
// Wind ripples / sastrugi on the open snow. KEY: value noise sits on an integer lattice
// -> any direct use shows a GRID. Fix = DOMAIN WARP: displace the sample coords by noise,
// which destroys the lattice into organic flow (no grid). Coords are also rotated off-axis
// first. Wind ridges = a sine of the WARPED coord, so the ridge lines meander naturally.
float rippleH(vec2 w) {
    // NO sine ridges (they caused parallel lines -> then concentric rings when the
    // direction was varied). Just SOFT domain-warped dunes: organic undulation with no
    // grid, no lines, no rings. Coarse so it isn't a fine shimmer.
    const mat2 R = mat2(0.86, -0.51, 0.51, 0.86);     // off the world axes
    vec2 p = R * w * 0.6;
    vec2 warp = vec2(snowGrain(p), snowGrain(p + vec2(11.3, 4.7))) - 0.5;
    p += warp * 1.3;                                   // organic flow (kills the lattice)
    return fbmGrain(p);                                // soft dunes
}

layout(location = 0) in vec3  vWorldPos;
layout(location = 1) in vec3  vNormal;
layout(location = 2) in float vEdge;
layout(location = 3) in float vSlope;
layout(location = 0) out vec4 outColor;

void main() {
    vec3  geomN = normalize(vNormal);
    float cov   = clamp(L.sf_params.w, 0.0, 1.0);

    // DEBUG: flat magenta sheet (shaded by normal so dents are visible), only gated by
    // the grid rim + coverage. If this shows, the mesh draws -> any real-mode blankness
    // is the ground-map height / discard, not the pass itself.
    if (pc.p1.w > 0.5) {
        if (vEdge < 0.02 || cov < 0.02) discard;
        float sh = 0.35 + 0.65 * clamp(geomN.y, 0.0, 1.0);
        outColor = vec4(vec3(1.0, 0.0, 1.0) * sh, 1.0);
        return;
    }

    // Steep underlying surface (walls / fences / cliffs) -> no snow. Uses the height-field
    // slope (catches walls the smoothed shading normal would miss). Dents stay under it.
    float flatn = 1.0 - smoothstep(1.3, 2.2, vSlope);
    float expo  = rainVis(vWorldPos);                                  // open to sky?
    // Organic growth: noise-varied coverage onset so partial snow fills in irregular
    // patches, not a hard uniform line. Full coverage -> uniform.
    float covThr = (fbmGrain(vWorldPos.xz * 0.13) * 0.6 + fbmGrain(vWorldPos.xz * 0.45) * 0.4) * 0.75;
    float covM   = smoothstep(covThr, covThr + 0.22, cov);
    float snow   = covM * flatn * expo * vEdge;
    if (snow < 0.02) discard;

    vec3 albedo = vec3(0.90, 0.93, 0.97);
    float press = clamp(SnowDeformPress(vWorldPos), -1.0, 1.0);       // signed: + dent (down), - berm (up)

    // Dent gradient from the FINER deform field (~3.9 cm vs the ~15.7 cm mesh quad).
    float ew  = 0.04;
    float pX  = SnowDeformPress(vWorldPos + vec3(ew, 0.0, 0.0)) - SnowDeformPress(vWorldPos - vec3(ew, 0.0, 0.0));
    float pZ  = SnowDeformPress(vWorldPos + vec3(0.0, 0.0, ew)) - SnowDeformPress(vWorldPos - vec3(0.0, 0.0, ew));
    vec2  dentG = vec2(pX, pZ) * (pc.p1.x / (2.0 * ew));
    float wall  = clamp(length(dentG) * 1.6, 0.0, 1.0);              // 1 on a steep dent wall
    geomN = normalize(geomN + vec3(dentG.x, 0.0, dentG.y) * 1.5);

    // Wind-ripple relief on the WHOLE snow surface (esp. the bare plain) — catches the
    // sun and breaks the "smooth" look. World-space finite differences (correct normal)
    // + distance fade (procedural relief aliases at range).
    {
        float rfade = clamp(1.0 - distance(vWorldPos, L.eye_pos.xyz) / 28.0, 0.0, 1.0);
        // Patchiness: ripple strength varies place to place (some areas wind-scoured &
        // rippled, some smooth) so it isn't a uniform pattern. Two low-freq masks at
        // different scales -> bigger/smaller, present/absent.
        float pmask = snowGrain(vWorldPos.xz * 0.09) * 0.7 + snowGrain(vWorldPos.xz * 0.27) * 0.3;
        pmask = smoothstep(0.30, 0.80, pmask);
        float e = 0.12;
        float rx = rippleH(vWorldPos.xz + vec2(e, 0.0)) - rippleH(vWorldPos.xz - vec2(e, 0.0));
        float rz = rippleH(vWorldPos.xz + vec2(0.0, e)) - rippleH(vWorldPos.xz - vec2(0.0, e));
        geomN = normalize(geomN + vec3(rx, 0.0, rz) * (pc.p2.x * rfade * pmask * vEdge));   // fade at the mesh rim (no seam)
    }

    // ★ FLAT-LIGHT definition. Overcast snow is sky-ambient dominated, so normal detail
    // VANISHES — a print only reads through AMBIENT OCCLUSION + packed-snow albedo, with
    // a SHARP onset so the edge is crisp, not a gel bowl. This is the real "less liquid".
    float dentAO = smoothstep(0.03, 0.38, press);                    // press>0 = pressed-down dent
    albedo = mix(albedo, vec3(0.50, 0.56, 0.68), dentAO * 0.85);     // packed, darker, bluer
    albedo *= 1.0 + max(-press, 0.0) * 0.12;                         // berm (press<0) = fresh piled snow, brighter

    // Subtle low-freq surface variation everywhere (no aliasing) so the plain isn't a
    // dead-flat sheet.
    albedo *= 0.93 + 0.07 * fbmGrain(vWorldPos.xz * 4.0);
    albedo *= 0.97 + 0.03 * snowGrain(vWorldPos.xz * 0.6);

    // ★ LOOSE / GRANULAR disturbed snow (sand-like crumbs). ROTATED multi-octave noise
    // (fbmGrain — no axis-aligned grid, so NO tyre-tread pattern) + a DISTANCE FADE
    // (high-freq procedural detail aliases at range -> only show it up close). Gated by
    // the disturbance (print depth + walls): bright/dark crumbs + crevice AO + soft bumps.
    float distC   = distance(vWorldPos, L.eye_pos.xyz);
    float gfade   = clamp(1.0 - distC / 11.0, 0.0, 1.0);
    float disturb = clamp(abs(press) * 1.3 + wall * 0.8, 0.0, 1.0) * gfade;   // dents AND berms are loose
    float granule = fbmGrain(vWorldPos.xz * 17.0);                    // ~6 cm crumbs, rotated
    albedo *= 1.0 + (granule - 0.5) * 0.30 * disturb;                 // bright tops / dark pits

    {   // soft crumb bumps, ONLY in disturbed snow (faded by distance -> no tread)
        float e2 = 0.06;
        float cx = fbmGrain((vWorldPos.xz + vec2(e2, 0.0)) * 17.0) - fbmGrain((vWorldPos.xz - vec2(e2, 0.0)) * 17.0);
        float cz = fbmGrain((vWorldPos.xz + vec2(0.0, e2)) * 17.0) - fbmGrain((vWorldPos.xz - vec2(0.0, e2)) * 17.0);
        geomN = normalize(geomN + vec3(cx, 0.0, cz) * (1.1 * disturb));
    }

    float sunMask = max(dot(geomN, normalize(-L.sun_dir.xyz)), 0.0);
    if (sunMask > 0.005) sunMask *= sunShadow(vWorldPos);

    // Matte: snow is diffuse. A touch of extra sky fill ("airy" soft look), no spec term.
    vec3 lighting = skyAmbient(geomN) * (L.sky_params.y * 1.08)
                  + L.sun_color.rgb * sunMask
                  + L.ambient.rgb;
    vec3 col = albedo * lighting;

    // Occlusion in the dip + a darker line on the steep walls -> crisp print in flat light.
    col *= 1.0 - dentAO * 0.32;
    col *= 1.0 - wall   * 0.24;
    // crevice AO between loose grains (self-shadowing of crumbled snow).
    col *= 1.0 - (1.0 - granule) * 0.18 * disturb;

    // Sparkle: rare crystals catching the sun, ONLY on undisturbed crust (loose snow is
    // matte), softened + distance-faded so it doesn't shimmer.
    float sparkle = pow(snowGrain(vWorldPos.xz * 57.0), 24.0);
    col += L.sun_color.rgb * (sparkle * sunMask * 0.35 * (1.0 - disturb) * clamp(1.0 - distC / 18.0, 0.0, 1.0));

    float fog = clamp(length(vWorldPos - L.eye_pos.xyz) * L.fog_params.w + L.fog_params.x, 0.0, 1.0);
    col = mix(col, L.fog_color.rgb, fog);
    outColor = vec4(col, 1.0);
}
