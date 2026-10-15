#version 460
#extension GL_GOOGLE_include_directive : require
// ===========================================================================
//  xrRenderVulkan — DDGI PROBE DEBUG VIEW: draw the probes themselves.
// ===========================================================================
//  Six vertices per probe, no vertex buffer: everything a dot needs is already on
//  the GPU (the constants block says where the lattice is, the atlas strip says
//  how far each probe has moved, the atlas itself says what it knows). The CPU
//  cannot draw these — it does not have the offsets.
//
//  ⭐WHY THIS VIEW EXISTS AT ALL. Relocation, the Chebyshev test and "GI does
//  nothing indoors" are all statements about WHERE THE PROBES ARE, and until now
//  the only instrument for that was a percentage averaged over 2048 probes — which
//  printed 0.0% for a room whose entire question was three of them. A number that
//  coarse cannot answer "is there a probe in this room", and a screenshot can.
//
//  r_ddgi_show_probes: 1 = colour by what the probe KNOWS (its irradiance towards
//  the camera), 2 = colour by its STATE (see below).
//
//  The dot keeps a constant SCREEN size (the offset is scaled by clip.w), which is
//  deliberate: a probe 20 m away has to stay visible, because the question is
//  usually "how far do they reach" rather than "how big is this one".

#define DDGI_SET      0
#define DDGI_B_CONSTS 0
#define DDGI_B_IRR    1
#define DDGI_B_DEP    2
#define DDGI_SAMPLE
#include "ddgi_common.glsl"

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 cam;    // xyz = camera position, w = draw radius (m)
    vec4 opts;   // x = mode, y = dot size (screen fraction), z = ray scale, w = ray radius
} pc;

layout(location = 0) out vec3 vCol;
layout(location = 1) out vec2 vUV;

// Blue → green → red by luminance. Mode 1 shows the probe's colour, which answers
// "what does it see"; this answers "how much", and the two are not the same
// question. Scaling colour and tone mapping it saturates to white the moment the
// scale is high enough to see anything at all — which is exactly what happened on
// the first indoor screenshot, where every probe went white together and the
// gradient the room was being judged on disappeared into the clipping.
vec3 powerRamp(float x)
{
    x = clamp(x, 0.0, 1.0);
    return (x < 0.5) ? mix(vec3(0.05, 0.1, 0.8), vec3(0.1, 0.9, 0.2), x * 2.0)
                     : mix(vec3(0.1, 0.9, 0.2), vec3(1.0, 0.25, 0.05), (x - 0.5) * 2.0);
}

void main()
{
    const int vid    = gl_VertexIndex;
    const int gp     = vid / 6;
    const int corner = vid - gp * 6;

    // Undo ddgiGlobalProbe: the atlas is one flat run per cascade.
    const int perC = max(ddgi.casc.y, 1);
    const int c    = gp / perC;
    const int slot = gp - c * perC;

    const ivec3 absC = ddgiSlotToAbs(ddgiSlotCoord(slot), ddgi.base[c].xyz);
    const vec3  P    = ddgiProbePos(absC, c, gp);

    // ⚠ A cascade that has never been measured is PARKED at base = 1<<24, i.e. a
    // position millions of metres away. The radius test below is what keeps those
    // out of the view; without it they would still be drawn, just as a dot at the
    // horizon that means nothing.
    if (distance(P, pc.cam.xyz) > pc.cam.w) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }

    // The relocation strip: xyz = offset in metres, w = flags (1 moved, 2 buried).
    const vec4 st    = texelFetch(ddgiIrrTex, ddgiOffsetTexel(gp), 0);
    const int  flags = int(round(st.w));

    vec3 col;
    if (pc.opts.x < 1.5) {
        // What this probe would answer a surface facing the camera — i.e. the thing
        // the lighting actually reads out of it. A black dot here IS the bug being
        // hunted: a probe inside geometry converges to nothing.
        const vec3 toCam = normalize(pc.cam.xyz - P);
        col = texture(ddgiIrrTex, ddgiTileUV(gp, toCam, DDGI_IRR_N, vec2(textureSize(ddgiIrrTex, 0)))).rgb;
        // ⚠ SCALED BEFORE THE TONE MAP, and this is not a cosmetic knob. Indoors the
        // whole question is whether the probes near a window are BRIGHTER than the
        // ones in the corner — a difference of a few hundredths, which c/(1+c) maps
        // to indistinguishable near-black. r_ddgi_show_rays_scale drives it (the
        // rays view already had it), so "turn it up until the gradient appears" is
        // one command rather than a rebuild.
        col *= max(pc.opts.z, 0.001);
        col = col / (col + vec3(1.0));          // just enough tone map to stay visible in HDR
    } else if (pc.opts.x > 2.5) {
        // Mode 3 — HOW MUCH, as a ramp. Same irradiance as mode 1, reduced to
        // luminance and scaled by r_ddgi_show_rays_scale, so a difference of a few
        // hundredths between a probe by the window and one in the corner separates
        // into two visibly different colours instead of two shades of near-black
        // (or, once scaled up, two identical whites).
        const vec3 toCam = normalize(pc.cam.xyz - P);
        const vec3 e = texture(ddgiIrrTex, ddgiTileUV(gp, toCam, DDGI_IRR_N, vec2(textureSize(ddgiIrrTex, 0)))).rgb;
        col = powerRamp(dot(e, vec3(0.2126, 0.7152, 0.0722)) * max(pc.opts.z, 0.001));
    } else {
        // State. Red = the pass thinks this probe is inside geometry, yellow = it
        // moved on the last update, blue = it sits displaced and settled, green =
        // untouched on its lattice point, grey = classified inactive (no geometry
        // in its cell box; r_ddgi_classify).
        const bool buried   = (flags & DDGI_FLAG_BURIED) != 0;
        const bool moved    = (flags & DDGI_FLAG_MOVED) != 0;
        const bool inactive = (flags & DDGI_FLAG_INACTIVE) != 0;
        const bool off      = dot(st.xyz, st.xyz) > 1e-4;
        col = buried   ? vec3(1.0, 0.1, 0.1)
            : moved    ? vec3(1.0, 0.8, 0.1)
            : inactive ? vec3(0.45, 0.45, 0.45)
            : off      ? vec3(0.2, 0.5, 1.0)
                       : vec3(0.15, 0.9, 0.25);
    }
    vCol = col;

    // Two triangles, in clip space. Scaling by w keeps the dot the same size on
    // screen at any distance.
    const vec2 quad[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                 vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    const vec2 q = quad[corner];
    vUV = q;

    vec4 clip = pc.viewProj * vec4(P, 1.0);
    clip.xy += q * pc.opts.y * clip.w;
    gl_Position = clip;
}
