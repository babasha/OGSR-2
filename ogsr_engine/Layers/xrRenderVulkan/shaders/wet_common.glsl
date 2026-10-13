// xrRenderVulkan — shared rain/puddle helpers.
//
// #included by every shader that paints wet ground: world_terrain / world_lmap /
// world_vlit (the surface wetness) and tonemap (the SSR puddle placement). These
// were copy-pasted across all four and drifted out of sync; keeping the single
// source here is what makes a ripple/puddle tweak a ONE-file change.
//
// All functions are PURE (no UBO / no texture bindings) so they can live in one
// header regardless of each shader's own descriptor layout. Include AFTER `#version`.
//
// Two procedural stand-ins for SSFX assets we don't ship on SoC levels:
//   • puddlesMaskProc — the per-level artist `s_puddles_mask` (WHERE puddles sit)
//   • rainRipples     — the `s_rainsplash` ring texture (drop-impact ripples)

// ---- value-noise (for the puddle mask) ------------------------------------
float vHash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float vNoise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(vHash(i), vHash(i + vec2(1.0, 0.0)), f.x),
               mix(vHash(i + vec2(0.0, 1.0)), vHash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// PuddlesMask stand-in: distinct organic puddle BODIES from a domain-warped value-
// noise FBM. `coverage` (0..1, driven by the wetness accumulator) lowers the
// threshold so puddles GROW as it rains and RECEDE as it dries; the DOMAIN WARP
// rounds off the value-noise's blocky diamond iso-contours; the wide smoothstep
// gives the soft SSS-style edge. `scale` (r_puddle_scale) sets the puddle size.
float puddlesMaskProc(vec2 xz, float coverage, float scale)
{
    float fq = 0.18 * scale;
    vec2  p  = xz * fq;
    vec2 w = vec2(vNoise(p * 0.5 + 3.1), vNoise(p * 0.5 + 8.7)) - 0.5;
    p += w * 2.2;
    float n = vNoise(p)              * 0.55
            + vNoise(p * 2.1 + 19.1) * 0.30
            + vNoise(p * 4.3 + 47.7) * 0.15;
    // coverage 0 → high thr (dry, no puddles); rises → thr drops → puddles grow.
    // Min 0.46 keeps the fullest state DISTINCT (no merged "fields of water").
    float thr = mix(0.84, 0.46, clamp(coverage, 0.0, 1.0));
    return smoothstep(thr, thr + 0.22, n);
}

// ---- rain-splash ripples (Lagarde-style drop rings) -----------------------
// One SMALL expanding ring per grid cell, fired on a per-cell phase so drops are
// SPARSE and brief (not a constant "boil"); the ring dissolves over a long smooth
// tail as it expands (no hard cutoff). Returns an xz normal perturbation. p in
// world metres. (The salts are also the build's authorship watermark — keep them.)
vec2 rippleLayer(vec2 p, float t)
{
    vec2 cell = floor(p);
    vec2 f = p - cell;
    const vec2  SALT_ZEFIR   = vec2(127.1, 311.7);
    const vec2  SALT_CATARA  = vec2(269.5, 183.3);
    const float SALT_SARATOV = 43758.5453;
    float h1 = fract(sin(dot(cell, SALT_ZEFIR))  * SALT_SARATOV);
    float h2 = fract(sin(dot(cell, SALT_CATARA)) * SALT_SARATOV);
    vec2  c  = vec2(0.25) + 0.5 * vec2(h1, h2);            // drop centre in the cell
    float ph = fract(t * (0.7 + 0.6 * h2) + h1);           // each drop's own slow cycle
    float d  = length(f - c);
    float radius = ph * 0.26;                              // small ring, expands with phase
    float front  = d - radius;
    float ring   = sin(clamp(front * 36.0, -3.14159, 3.14159)) * smoothstep(0.08, 0.0, abs(front));
    float life   = smoothstep(0.0, 0.05, ph) * (1.0 - smoothstep(0.25, 0.85, ph));  // dissolve, no cutoff
    return (d > 1e-4 ? (f - c) / d : vec2(0.0)) * (ring * life);
}

vec2 rainRipples(vec2 p, float t)
{
    // Two BIG-cell layers (~1.2 m / ~0.8 m spacing) → sparse rings.
    return rippleLayer(p * 0.85,                  t * 0.9)
         + rippleLayer(p * 1.3 + vec2(0.5, 0.25), t * 1.15) * 0.6;
}
