// xrRenderVulkan - diffuse sky ambient (irradiance), shared.
//
// Split out of env_common.glsl so grass / trees / skinned can use the SAME code:
// env_common depends on shadow_common (rainVis needs cascTap), which those stages
// do not include, so they had each grown a private copy of skyAmbient — and each
// copy had drifted (grass and trees hardcoded straight up, skinned dropped the
// cross-fade early-out). This header needs only light_ubo.glsl + sky_halfcube.glsl.
//
// #include AFTER light_ubo.glsl.
#ifndef SKY_AMBIENT_GLSL
#define SKY_AMBIENT_GLSL

#include "sky_halfcube.glsl"
// The SH9 closed form itself lives in sky_sh.glsl. The DDGI probe trace needs the
// SAME sky evaluated from its OWN descriptor set, and a second copy of these five
// constants is precisely how the two would drift apart without anyone noticing.
#include "sky_sh.glsl"

// shIrradianceC(n) — SH9 irradiance in direction n, from the env set's coefficient
// buffer (binding 31). Called directly: a forwarding wrapper here would show up in
// every consumer as a real extra OpFunctionCall (the SPIR-V gate says so).

// The diffuse irradiance arriving at a surface with normal N, as a radiance to
// multiply albedo by. The fill light that lights surfaces the sun never reaches —
// and at dusk, when the sun is under the horizon, essentially the ONLY light,
// which is why its directionality decides whether terrain reads as shaped or flat.
//
// PRIMARY — SH9. Irradiance from a distant environment is low-frequency enough
// that 9 coefficients reproduce it to ~1%, so the whole sky collapses into a few
// MADs here. Two properties matter beyond the speed: it is a real cosine-weighted
// INTEGRAL (so the horizon band carrying the sunset actually contributes, weighted
// by how much sky it subtends), and it is smooth by construction (so a per-pixel
// detail normal can be fed in without aliasing — which the old point-sampled path
// could not survive).
//
// FALLBACK — the prefiltered world-space probe's top mip. Used before the first
// projection lands and when r_sky_sh is off (A/B). Still far better than the
// original raw fetch: the probe has the half-cube remap and sky_rotation baked in,
// so its azimuth agrees with the visible sky.
//
// LAST RESORT — the raw weather cubes, remapped here. Only reached when vk_ibl
// never came up at all.
#define SH_U(i) L.sky_sh[i].rgb
vec3 shIrradianceU(vec3 n) SH_IRRADIANCE_EVAL(n, SH_U)   // the UBO mirror (cbuf), valid when sh_params.w == 1

vec3 skyAmbient(vec3 N)
{
    if (L.sh_params.x > 0.001) {
        if (L.sh_params.w > 0.5)
            return shIrradianceU(N) * L.sh_params.x;
        return shIrradianceC(N) * L.sh_params.x;
    }

    if (L.ibl_params.x > 0.004)
        return textureLod(uSkySpec, N, L.sh_params.z).rgb;

    // The weather cubes are authored in half-cube space, so the direction MUST be
    // remapped — sampling them with a plain world normal sends every up-facing
    // surface to the zenith texel and makes the horizon (i.e. the entire sunset)
    // unreachable.
    vec3  sdir = SkySampleDir(N, L.sh_params.y);
    float lod  = L.sky_params.z;
    float xf   = clamp(L.sky_params.x, 0.0, 1.0);   // weather cross-fade - usually 0/1
    vec3  a    = textureLod(uSky0, sdir, lod).rgb;
    return (xf > 0.01) ? mix(a, textureLod(uSky1, sdir, lod).rgb, xf) : a;
}

// Grass / tree blades have no meaningful shading normal, so they take the ambient
// straight up. Kept as a named entry point (rather than skyAmbient(vec3(0,1,0)) at
// every call site) because it marks a deliberate approximation, not an oversight.
vec3 skyAmbientUp()
{
    return skyAmbient(vec3(0.0, 1.0, 0.0));
}

#endif // SKY_AMBIENT_GLSL
