// xrRenderVulkan — pure shading/screen math with NO descriptor dependencies.
//
// Same contract as ao_common.glsl: every function here is a pure function of its
// arguments, so any stage can include it regardless of which descriptor set it
// binds — or whether it binds any at all. Anything that touches `L`, a sampler or
// a push block does NOT belong here (see ao_sampled.glsl for the AO/IL taps and
// env_common.glsl for the terms that need the shadow maps).
#ifndef COMMON_MATH_GLSL
#define COMMON_MATH_GLSL

// Split-sum environment BRDF approximation (Karis, "Mobile Optimized Lighting",
// SIGGRAPH'14) — the analytic stand-in for the split-sum BRDF LUT. Used by every
// surface that reflects the prefiltered probe: opaque world, grass, foliage.
vec3 EnvBRDFApprox(vec3 F0, float roughness, float NoV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572,  0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.040, -0.040);
    vec4  r    = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2  ab   = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * ab.x + ab.y;
}

// Clip-space NDC -> screen UV, with the Y flip this renderer's targets use.
// Shared by the motion-vector fragment shaders, which all resolve the same
// cur/prev clip pair into a screen-space delta.
vec2 ndc2uv(vec2 n) { return vec2(n.x * 0.5 + 0.5, 0.5 - 0.5 * n.y); }

#endif // COMMON_MATH_GLSL
