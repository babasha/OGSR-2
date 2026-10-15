#ifndef NX_SUN_SHADOW_GLSL
#define NX_SUN_SHADOW_GLSL
// nfs43 (Switch): the live sun shadow for the R1 receivers -- cascade 0, then cascade 1, then the cached far map, one
// 2x2 gather (cascTap) each, the same order/biases as the R1 terrain's sunShadowFast. cov = how much a live map covers
// this point (fades to 0 over the last 5 % of the far map), so callers can fall back to their baked term beyond it.
// Needs light_ubo.glsl (L, uShadow b1, uShadowNear b4, uShadowC1 b5) and shadow_math.glsl (cascTap) included first.
// nfs47: casters are culled only kCascadeZCull metres down-sun (vk_sun_cascades_cfg.h); past that a cascade is not
// trusted and the point falls to the next one. = SunCfg::CullNdc(60) and CullNdc(150) over the -160..460 m range.
#define NX_C0_ZMAX 0.3548387
#define NX_C1_ZMAX 0.5
float nxSunShadowCov(vec3 wp, out float cov)
{
    cov = 1.0;
    vec3 n  = (L.sun_near_vp * vec4(wp, 1.0)).xyz;
    vec2 uv = vec2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
    if (uv.x >= 0.01 && uv.x <= 0.99 && uv.y >= 0.01 && uv.y <= 0.99 && n.z > 0.0 && n.z < NX_C0_ZMAX)
        return cascTap(uShadowNear, uv, n.z - 0.0004);
    n  = (L.sun_c1_vp * vec4(wp, 1.0)).xyz;
    uv = vec2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
    if (uv.x >= 0.01 && uv.x <= 0.99 && uv.y >= 0.01 && uv.y <= 0.99 && n.z > 0.0 && n.z < NX_C1_ZMAX)
        return cascTap(uShadowC1, uv, n.z - 0.0006);
    vec4 c = L.sun_vp * vec4(wp, 1.0);
    if (c.w <= 0.0) { cov = 0.0; return 1.0; }
    vec3 ndc = c.xyz / c.w;
    uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) { cov = 0.0; return 1.0; }
    cov = clamp(min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) * 20.0, 0.0, 1.0);
    return cascTap(uShadow, uv, ndc.z - 0.0015);
}
#ifdef NX_SUNMASK_READ
// nfs46: read the half-res sun mask (binding 14, vsm_sample.glsl uVsmMask) at a full-res pixel: of the 4 nearest texels
// take the one whose stored depth is closest to this pixel's, so a shadow never leaks across a silhouette.
float nxSunMaskShadow(vec2 fragXY, float fragZ, out float cov)
{
    const vec2 uv = fragXY * 0.5 / vec2(textureSize(uVsmMask, 0));
    const vec4 s  = textureGather(uVsmMask, uv, 0);
    const vec4 c  = textureGather(uVsmMask, uv, 1);
    const vec4 dz = abs(textureGather(uVsmMask, uv, 2) - vec4(fragZ));
    float best = dz.x, sh = s.x; cov = c.x;
    if (dz.y < best) { best = dz.y; sh = s.y; cov = c.y; }
    if (dz.z < best) { best = dz.z; sh = s.z; cov = c.z; }
    if (dz.w < best) { best = dz.w; sh = s.w; cov = c.w; }
    return sh;
}
#endif
#endif
