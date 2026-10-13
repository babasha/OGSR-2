#ifndef SKY_SH_GLSL
#define SKY_SH_GLSL
// xrRenderVulkan — SH9 sky irradiance, the closed form, ONE copy.
//
// Split out of sky_ambient.glsl when the DDGI probes started needing the same
// sky: a probe ray that misses has to come home with the value the analytic path
// would have given that direction, or the probe field and the sky fill it
// REPLACES disagree about how bright the world is, and the difference reads as
// "turning GI on changed the exposure" rather than as GI.
//
// The two consumers bind their coefficients in different descriptor sets (the
// world's env set vs. the DDGI trace's own), which is exactly the situation that
// grows a private copy and then silently misses every later fix — the copy trap
// this renderer has already paid for once (see skinned.frag vs shadow_common).
// So the SOURCE of the coefficients is a macro and the maths is shared.
//
// Define SH_C(i) -> vec3 before including if the coefficients do not live in a
// block named `skySH` with a `c[9]` member (light_ubo.glsl's layout).

#ifndef SH_C
#define SH_C(i) skySH.c[i].rgb
#endif

// Ramamoorthi & Hanrahan's closed form; the c-constants already fold in the
// cosine-lobe convolution (A0/A1/A2), and the final 1/pi turns irradiance E into
// the radiance that multiplies albedo.
// The closed form as a macro over the coefficient source C(i), so a second source (sky_ambient.glsl: the UBO mirror)
// reuses the SAME maths instead of growing a copy. A truncated SH can ring negative where the environment is very
// directional (a low sun against a dark sky) -- clamp, or dusk prints black patches on slopes facing away from the glow.
#define SH_IRRADIANCE_EVAL(n, C)                                                    \
{                                                                                   \
    const float c1 = 0.429043, c2 = 0.511664, c3 = 0.743125,                        \
                c4 = 0.886227, c5 = 0.247708;                                       \
    float x = n.x, y = n.y, z = n.z;                                                \
    vec3 E = c4 * C(0)                                                              \
           + 2.0 * c2 * (C(3) * x + C(1) * y + C(2) * z)                            \
           + c3 * C(6) * (z * z)                                                    \
           - c5 * C(6)                                                              \
           + 2.0 * c1 * (C(4) * x * y + C(7) * x * z + C(5) * y * z)                \
           + c1 * C(8) * (x * x - y * y);                                           \
    return max(E * (1.0 / 3.14159265), vec3(0.0));                                  \
}

vec3 shIrradianceC(vec3 n) SH_IRRADIANCE_EVAL(n, SH_C)

#endif // SKY_SH_GLSL
