// nx_fog.glsl (10-10) -- the colour the distance fog fades a point into: the SKY behind it (r_fog_sky, Switch preset).
// The weather cube along the view ray x the sky draw's tint (sky.frag legacy path: sky_color x 1.7), with the sky draw's
// horizon band in fog_color (r_sky_fog_horizon) -- so a fully fogged point is the sky it stands in front of. With
// r_fog_dist 1 the fog closes at the far plane, and the plane clip sweeps the edge of the world as the camera turns (it
// reaches further at the screen edges): against the flat fog_color a fogged tree on a hill was a grey (120,129,136)
// silhouette popping in on a sky of (174,178,188). One cube tap (two in a weather cross-fade) on a coarse mip, only where
// there is fog. Off (fog_sky.w == 0, the PC default): the flat fog_color, as before.
// CONTRACT: after light_ubo.glsl (L, uSky0, uSky1). Callable from the vertex stage (the env set exposes 6/7 to the VS).
#ifndef NX_FOG_GLSL
#define NX_FOG_GLSL

#include "sky_halfcube.glsl"

// v320: mip 5 of the 1024 weather cubes = 32 texels a face (~3 deg each): the sky's colour behind a fogged crown, not its
// cloud detail, and a face the size of the texture cache -- at mip 2 (256) nearly every fogged pixel missed it (+0.28 ms
// looking into thick fog, v319).
#define NX_FOG_SKY_LOD 5.0

// fog = this point's fog amount. v320: the sky only takes over where the fog is thick (NX_FOG_SKY_FROM..TO): the thin
// haze of the middle distance keeps the flat fog_color (in it the two read the same) and skips the tap altogether, and
// what the clip sweeps -- fog 1 -- is still exactly the sky.
#define NX_FOG_SKY_FROM 0.35
#define NX_FOG_SKY_TO   0.75

vec3 nxFogColor(vec3 wp, float fog)
{
    // 📏v320 A/B (rainy village, standing): r_fog_sky 0 7.47 ms | v319 (mip 2, every fogged pixel) 8.16 | this 7.64 --
    // the fog into the sky +0.69 -> +0.17 ms.
    if (L.fog_sky.w <= 0.0 || fog <= NX_FOG_SKY_FROM)
        return L.fog_color.rgb;
    const vec3  d  = normalize(wp - L.eye_pos.xyz);
    const vec3  sd = SkySampleDir(d, L.sh_params.y);   // the half-cube remap + sky_rotation, as skyAmbient's raw path
    vec3 s = textureLod(uSky0, sd, NX_FOG_SKY_LOD).rgb;
    const float xf = clamp(L.sky_params.x, 0.0, 1.0);   // weather cross-fade
    if (xf > 0.01)
        s = mix(s, textureLod(uSky1, sd, NX_FOG_SKY_LOD).rgb, xf);
    const vec3 sky = mix(s * L.fog_sky.rgb, L.fog_color.rgb, 1.0 - smoothstep(0.0, L.fog_sky.w, d.y));
    return mix(L.fog_color.rgb, sky, smoothstep(NX_FOG_SKY_FROM, NX_FOG_SKY_TO, fog));
}

#endif
