#ifndef DDGI_SKY_GLSL
#define DDGI_SKY_GLSL
// xrRenderVulkan — THE SKY, AS A RAY SEES IT. One copy, two consumers.
//
// Split out of ddgi_trace.comp.glsl when the final gather (vk_pass_gather) needed
// exactly the same two answers. A private copy of this would be the copy trap this
// renderer has already paid for twice: the gather REPLACES the probe lookup in the
// ambient, so if the two disagreed about the sky by any factor at all, switching
// the gather on would read as an exposure change and no A/B could tell whether the
// gather itself was any good.
//
// ⚠ TWO SKY QUERIES, AND CONFUSING THEM COSTS ~10% OF THE AMBIENT.
//
// A MISSED RAY needs the sky's RADIANCE along that one direction. A HIT needs the
// sky's IRRADIANCE for the hit surface's normal — the same quantity skyAmbient(N)
// hands that surface in the raster pass.
//
// Feeding the SH irradiance to a missed ray (the obvious shortcut, since it is
// already here for the hits) convolves the sky with a cosine lobe TWICE: once in
// the SH itself, and again when the consumer cosine-averages the rays. A double
// convolution is not a small smoothing — it attenuates the l=1 band to 2/3 and
// l=2 to 1/4, i.e. it drags the answer toward the zenith. At dawn the zenith is
// the DARKEST part of the sky, so every up-facing surface came out systematically
// dark, and the whole GI swap read as "turning it on dims the world". Measured on
// Pripyat at 06:00: ~-4 of 255 frame-wide, far more on the road. The probe cube is
// the right source for a ray: it IS radiance.
//
// Requires: ddgi_common.glsl included first (the `ddgi` constants block), and the
// includer to define DDGI_SET / DDGI_B_SKYSH / DDGI_B_SKYCUBE.

// The sky SH9 coefficients (vk_ibl's projection, the same buffer the env set binds
// at 31) and the prefiltered world-space sky probe those coefficients were
// projected FROM (the env set's binding 26). Both are bound to placeholders until
// vk_ibl has content; layout_.y / layout_.z gate them.
layout(std430, set = DDGI_SET, binding = DDGI_B_SKYSH) readonly buffer DDGISkySH { vec4 c[9]; } ddgiSH;
layout(set = DDGI_SET, binding = DDGI_B_SKYCUBE) uniform samplerCube ddgiSkyCube;
#define SH_C(i) ddgiSH.c[i].rgb
#include "sky_sh.glsl"

vec3 ddgiSkyRadiance(vec3 d)
{
    if (ddgi.layout_.z != 0) return textureLod(ddgiSkyCube, d, 1.0).rgb * ddgi.sunColor.w;
    if (ddgi.layout_.y != 0) return shIrradianceC(d) * ddgi.sunColor.w;
    return ddgi.skyColor.rgb;
}

vec3 ddgiSkyIrradiance(vec3 n)
{
    if (ddgi.layout_.y != 0) return shIrradianceC(n) * ddgi.sunColor.w;
    return ddgi.skyColor.rgb * (0.5 + 0.5 * n.y);   // the flat hemisphere lobe of rt_shade
}

#endif // DDGI_SKY_GLSL
