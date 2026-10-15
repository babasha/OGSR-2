#version 460
#extension GL_GOOGLE_include_directive : require
// ===========================================================================
//  xrRenderVulkan — DDGI RAY DEBUG VIEW: the rays a probe actually cast, drawn
//  as line segments coloured by the radiance they brought home.
// ===========================================================================
//  Two vertices per ray, no vertex buffer. The segment runs from the probe to
//  where the ray stopped, so its LENGTH is the hit distance and its COLOUR is the
//  radiance that gets folded into the octahedral map — i.e. you are looking at the
//  integrand, not at a visualisation of it.
//
//  ⭐WHAT THIS ANSWERS THAT NOTHING ELSE DOES. "The room is evenly lit" and "the
//  probes see nothing" and "the probes see plenty but the receiver drops it" all
//  look identical in the frame. Here they do not: short rays in every direction =
//  a buried probe; long rays coming back black = geometry with no light on it;
//  bright rays out of the window = the field HAS the answer and the loss is
//  downstream.
//
//  ⚠ ONE CASCADE ONLY, AND IT IS THE ONE UPDATED THIS FRAME. The ray buffer holds
//  a single cascade's worth of work (that is the whole point of the interleaving),
//  so with several cascades the view alternates between them frame to frame. That
//  flicker is not a bug in the view, it is the update schedule made visible.
//
//  r_ddgi_show_rays: 1 = radiance as it is (tone mapped to stay visible),
//                    2 = POWER ramp — luminance mapped blue→green→red, which reads
//                        far better when the question is "how much", not "what hue".

#define DDGI_SET      0
#define DDGI_B_CONSTS 0
#define DDGI_B_IRR    1
#define DDGI_B_DEP    2
#define DDGI_SAMPLE
#include "ddgi_common.glsl"

layout(std430, set = 0, binding = 3) readonly buffer DDGIRays { vec4 rayIn[]; };

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 cam;    // xyz = camera, w = probe draw radius (m)
    vec4 opts;   // x = mode, y = dot size, z = radiance scale, w = RAY draw radius (m)
} pc;

layout(location = 0) out vec3 vCol;

// Blue (dark) → green → red (bright). Deliberately not a hue rotation of the
// radiance: the eye reads a ramp quantitatively and a hue qualitatively.
vec3 powerRamp(float x)
{
    x = clamp(x, 0.0, 1.0);
    return (x < 0.5) ? mix(vec3(0.05, 0.1, 0.8), vec3(0.1, 0.9, 0.2), x * 2.0)
                     : mix(vec3(0.1, 0.9, 0.2), vec3(1.0, 0.25, 0.05), (x - 0.5) * 2.0);
}

void main()
{
    const int rays = max(ddgi.dims.w, 1);
    const int vid  = gl_VertexIndex;
    const int seg  = vid / 2;             // which ray
    const int end  = vid - seg * 2;       // 0 = at the probe, 1 = at the hit

    const int probe = seg / rays;         // slot WITHIN the updated cascade
    const int r     = seg - probe * rays;

    const int c  = ddgi.layout_.w;        // the cascade whose rays are in the buffer
    const int gp = ddgiGlobalProbe(c, probe);

    const ivec3 absC = ddgiSlotToAbs(ddgiSlotCoord(probe), ddgi.base[c].xyz);
    const vec3  P    = ddgiProbePos(absC, c, gp);

    // Rays are dense (64 per probe), so their radius is its own, smaller knob —
    // otherwise one step into a room draws a quarter of a million segments.
    if (distance(P, pc.cam.xyz) > pc.opts.w) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }
    // The fixed relocation rays carry no radiance and would draw as black spokes
    // over the ones that do; this view is about what the blends integrate.
    if (r < ddgiFixedRays()) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }
    // A classified-inactive probe that was not refreshed this update traced no
    // rotated rays: its run of the buffer still holds another probe's last rays.
    if (!ddgiSlotIsNew(absC, c) && ddgiSkipInactive(probe, int(round(ddgiProbeState(gp).w)))) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return;
    }

    const vec4  s   = rayIn[probe * rays + r];
    const vec3  dir = ddgiProbeRayDir(r);
    const float len = max(s.a, 0.0);

    const vec3 W = (end == 0) ? P : (P + dir * len);

    const vec3 rad = s.rgb * max(pc.opts.z, 0.0);
    if (pc.opts.x < 1.5) {
        vCol = rad / (rad + vec3(1.0));
    } else {
        vCol = powerRamp(dot(rad, vec3(0.2126, 0.7152, 0.0722)));
    }
    // Dim the probe end so each segment shows its DIRECTION, not just its colour.
    if (end == 0) vCol *= 0.15;

    gl_Position = pc.viewProj * vec4(W, 1.0);
}
