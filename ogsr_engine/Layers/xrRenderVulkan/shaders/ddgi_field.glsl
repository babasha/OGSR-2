#ifndef DDGI_FIELD_GLSL
#define DDGI_FIELD_GLSL
// xrRenderVulkan — the probe field WITHOUT the screen-space parts, so it can be
// sampled from a VERTEX shader.
//
// WHY THIS SPLIT EXISTS (measured 22-08-2026)
// ------------------------------------------
// ddgi_receiver.glsl answers a SURFACE's question and reads two screen-space
// things on the way: the final gather (binding 37) and gl_FragCoord. That makes
// it fragment-only, which is exactly right for the world — one shaded fragment
// per pixel — and exactly wrong for FOLIAGE.
//
// 📏Canopies are alpha-tested cards with heavy overdraw: putting the eight-probe
// lookup (16 atlas taps) on every fragment took the `Trees` pass from 2.2 ms to
// 11.9 ms and doubled the whole frame, 10 -> 22.5 ms. The ambient a crown wants is
// low-frequency by nature, so it belongs on the VERTEX, where the count is orders
// of magnitude smaller and the interpolator delivers the vertical gradient that
// gives the canopy its depth for free.
//
// The fragment still applies the parts that are genuinely per-pixel — the gather
// blend and the baked scalar — through ddgiAmbientFromField() in ddgi_receiver.
//
// #include AFTER light_ubo.glsl. Define DDGI_SET/DDGI_B_* first if not ENV_SET.
#ifndef DDGI_SET
#define DDGI_SET      ENV_SET
#define DDGI_B_CONSTS 34
#define DDGI_B_IRR    35
#define DDGI_B_DEP    36
#endif
#define DDGI_SAMPLE
#include "ddgi_common.glsl"

// rgb = the field's answer at P, w = how much of the ambient it should own.
// w == 0 means "the field has nothing to say here" — outside the volume, or the
// feature is off — and every caller must fall back to its analytic fill.
vec4 ddgiFieldWeighted(vec3 P, vec3 N, vec3 V)
{
    float k = L.ddgi_params.x;
    if (k <= 0.0) return vec4(0.0);
    k *= ddgiVolumeFade(P);          // fades to the analytic fill at the volume edge
    if (k <= 0.0) return vec4(0.0);
    return vec4(ddgiSampleIrradiance(P, N, V), k);
}

#endif // DDGI_FIELD_GLSL
