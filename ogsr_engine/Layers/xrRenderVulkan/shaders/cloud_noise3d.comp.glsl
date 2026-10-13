#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan - bake the two tileable 3D noise volumes the cloud raymarch reads.
// Run ONCE at init (vk_clouds.cpp): the volumes are static, only the sampling
// position animates, so this never costs a frame.
//
// mode 0 -> SHAPE volume (128^3, RGBA8)
//   r = Perlin-Worley  : the base cloud form (connected wisps + billows)
//   gba = Worley at 3 rising frequencies : combined into an FBM that erodes r
// mode 1 -> DETAIL volume (32^3, RGBA8)
//   rgb = Worley at 3 rising frequencies : the fine cauliflower edge, applied only
//         where the base shape already has density (cheap, small volume)
//
// The split is Schneider's (Horizon Zero Dawn / Nubis): a big low-frequency volume
// decides WHERE cloud is, a small high-frequency one decides what its EDGE looks
// like. Doing it with one huge volume would cost far more memory for the same look.

#include "noise_common.glsl"

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, rgba8) uniform writeonly image3D uOut;

layout(push_constant) uniform Push {
    vec4 p;   // x = resolution, y = mode (0 shape / 1 detail), zw unused
} pc;

void main()
{
    ivec3 gid = ivec3(gl_GlobalInvocationID);
    int   res = int(pc.p.x);
    if (gid.x >= res || gid.y >= res || gid.z >= res) return;

    vec3 uvw = (vec3(gid) + 0.5) / float(res);
    vec4 outv;

    if (pc.p.y < 0.5) {
        // SHAPE
        outv.r = nc_perlinWorley(uvw, 4.0);
        outv.g = nc_worleyFbm(uvw,  4.0);
        outv.b = nc_worleyFbm(uvw,  8.0);
        outv.a = nc_worleyFbm(uvw, 16.0);
    } else {
        // DETAIL
        outv.r = nc_worleyFbm(uvw,  8.0);
        outv.g = nc_worleyFbm(uvw, 16.0);
        outv.b = nc_worleyFbm(uvw, 32.0);
        outv.a = 1.0;
    }

    imageStore(uOut, gid, clamp(outv, 0.0, 1.0));
}
