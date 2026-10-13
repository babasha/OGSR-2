#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require   // the RT diffuse array is indexed per invocation
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — RT PREVIEW: primary rays from the camera, shaded by rt_shade.
//
// This is not an effect. It is the instrument that proves the shadeable-hit path
// before anything depends on it: if the screen shows the world in roughly its own
// colours, with the sun casting shadows the raster agrees with, then normals,
// material albedo and the shadow ray are all correct — and the DDGI probe update
// can be written against a component that has already been seen to work.
//
// Debugging probes THROUGH an irradiance atlas would mean guessing which of four
// things was wrong from one blurry symptom, so the order is deliberate: make the
// hit shadeable and LOOK at it first.
//
// r_rtgi_debug: 1 = shaded, 2 = albedo, 3 = geometric normal, 4 = hit distance,
//               5 = DDGI irradiance at the hit, 6 = albedo * that (the bounce
//                   light as it will actually be added to the image).
//
// 5 and 6 are the instrument for the probe volume, and the reason it is worth
// having: an irradiance field is otherwise invisible until it is wired into the
// lighting, at which point a leak, a dark probe and a wrong blend all look like
// "the GI is off somehow". Sampled straight onto the primary hit they are
// separable — leaks are light where a wall is, dark probes are blotches that do
// not move with the camera, a wrong blend smears when you walk.
//
// What it will NOT show, by construction (all four are the BVH's known limits,
// not bugs in this shader): anything past the camera bubble (r_rtao_range, sky
// beyond it), foliage and grass (no collision), dynamic objects (static BLAS
// only), and invisible collision-only walls if r_rtao_filter dropped them.

#define RT_B_TLAS   0
#define RT_B_VERTS  1
#define RT_B_MATS   2
#define RT_B_ALBEDO 3
#define RT_TEX_SET  1     // vk_rt_world: the world diffuse array, when the BVH is render geometry
#include "rt_shade.glsl"

#define DDGI_SET      0
#define DDGI_B_CONSTS 4
#define DDGI_B_IRR    5
#define DDGI_B_DEP    6
#define DDGI_SAMPLE
#include "ddgi_common.glsl"

layout(location = 0) out vec4 outColor;

// 128 bytes exactly — the guaranteed push-constant minimum, so this needs no
// UBO. ⚠ Keep in lockstep with RTGIDebugPush in vk_pass_rtgi.cpp (std430 vs C++
// packing has bitten this renderer before: vulkan-push-constant-std430-mismatch).
layout(push_constant) uniform PC {
    vec4 camDir;      // xyz = forward (unit), w = primary ray range (m)
    vec4 camRightT;   // xyz = right * tan(fovX/2), w = tan(fovX/2)
    vec4 camTopT;     // xyz = top   * tan(fovY/2), w = tan(fovY/2)
    vec4 camPos;      // xyz = camera world position, w = shadow ray range (m)
    vec4 res;         // xy = target size, zw = 1 / size
    vec4 sunDir;      // xyz = direction TO the sun (unit)
    vec4 sunColor;    // rgb
    vec4 skyColor;    // rgb, w = debug mode
} pc;

void main()
{
    // GI-only reflectance scale (r_rt_albedo), shipped as an int x256 in the slot
    // that used to be reserved. Set before any trace: rtTraceClosest reads it.
    rtAlbedoBoost = float(ddgi.casc.w) * (1.0 / 256.0);
    // Ray from the camera through this pixel. Built from gl_FragCoord (framebuffer
    // space, top-left origin) with the same D3D-style NDC the rest of the renderer
    // uses, so it does not care whether the viewport was flipped.
    const vec2 uv  = (gl_FragCoord.xy + 0.0) * pc.res.zw;
    const vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    const vec3 dir = normalize(pc.camDir.xyz + pc.camRightT.xyz * ndc.x + pc.camTopT.xyz * ndc.y);

    const int mode = int(pc.skyColor.w + 0.5);

    RTHit h = rtTraceClosest(pc.camPos.xyz, dir, 0.05, pc.camDir.w);

    vec3 c;
    if (mode == 2) {
        c = h.hit ? h.albedo : vec3(0.0);
    } else if (mode == 3) {
        c = h.hit ? (h.nrm * 0.5 + 0.5) : vec3(0.0);
    } else if (mode == 4) {
        // Banded so the eye can read absolute distance, not just a gradient: one
        // stripe per 10 m, brightness falling off over the whole range.
        c = h.hit ? vec3(fract(h.dist * 0.1)) * (1.0 - h.dist / pc.camDir.w) : vec3(0.0);
    } else if (mode == 5 || mode == 6) {
        if (h.hit) {
            const vec3 E = ddgiSampleIrradiance(h.pos, h.nrm, -dir);
            c = (mode == 6) ? (h.albedo * E) : E;
        } else {
            c = vec3(0.0);
        }
    } else {
        c = rtShadeHit(h, normalize(pc.sunDir.xyz), pc.sunColor.rgb, pc.skyColor.rgb, pc.camPos.w);
    }

    outColor = vec4(c, 1.0);
}
