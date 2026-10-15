#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan - bake the cloud WEATHER MAP (512^2, RGBA8), once at init.
//
// This is what stops the sky being a uniform soup of cloud. It is a slowly-scrolling
// 2D field that says, per square kilometre of sky, HOW MUCH cloud there is and WHAT
// KIND — so you get banks and towers here, clear blue gaps there, which is what
// actually reads as a real sky.
//
//   r = coverage : 0 clear .. 1 solid overcast
//   g = type     : 0 flat stratus .. 1 towering cumulus (drives the height gradient)
//   b = a second, larger-scale coverage band, used to cluster clouds into systems
//       rather than sprinkling them evenly
//   a = 1
//
// Two decorrelated FBMs (offset sample positions) so coverage and type do not move
// together — otherwise every thick region is also every tall region, and the sky
// looks stamped from one pattern.

#include "noise_common.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba8) uniform writeonly image2D uOut;

layout(push_constant) uniform Push {
    vec4 p;   // x = resolution, yzw unused
} pc;

// 2D FBM via the 3D Perlin on a fixed slice (keeps one tileable implementation).
float fbm2(vec2 uv, float period, float slice)
{
    return nc_perlinFbm(vec3(uv, slice), period, 4);
}

void main()
{
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    int   res = int(pc.p.x);
    if (gid.x >= res || gid.y >= res) return;

    vec2 uv = (vec2(gid) + 0.5) / float(res);

    // Coverage: mid-frequency, contrast-stretched so there are genuine holes and
    // genuine banks instead of an even grey mush.
    float cov = fbm2(uv, 3.0, 0.17);
    cov = nc_remap(cov, 0.35, 0.85, 0.0, 1.0);

    // Cloud systems: a much larger-scale band that modulates coverage, so cloud
    // gathers into weather fronts rather than uniform speckle.
    float band = fbm2(uv, 1.0, 0.61);
    band = nc_remap(band, 0.30, 0.80, 0.25, 1.0);

    // Type: decorrelated from coverage (different slice + period).
    float typ = fbm2(uv, 2.0, 0.83);
    typ = nc_remap(typ, 0.30, 0.75, 0.0, 1.0);

    imageStore(uOut, gid, clamp(vec4(cov, typ, band, 1.0), 0.0, 1.0));
}
