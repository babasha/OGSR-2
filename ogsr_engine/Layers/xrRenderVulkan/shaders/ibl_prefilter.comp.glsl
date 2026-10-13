#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan - Sky specular IBL: resample the blended weather sky cubes into
// mip 0 of an RGBA16F "specular" cube. The roughness-mip chain is then produced
// by a box blit chain (vk_ibl.cpp) - mip N = rougher reflection. v1 is a straight
// resample + box mips (fast, seam-tolerant for a low-freq sky); the clean upgrade
// is a per-mip GGX importance-sample convolution (see the note in vk_ibl.cpp).
//
// The output cube is a TRUE WORLD-SPACE probe: each output texel's world
// direction is resolved through the SAME half-cube remap + sky_rotation the dome
// draw uses (sky_halfcube.glsl), so the probe and the visible sky agree texel for
// texel. That makes this cube the one place the sky is "unwrapped", and every
// consumer downstream — the specular reflection AND the diffuse SH projection —
// gets to index it with a plain world vector.
//
// This used to sample RAW, on the theory that "the half-cube remap is a draw-only
// thing; the light probe is raw". It is not draw-only: it is the mapping the
// cubemap is AUTHORED in. Sampling raw put the probe on a different sky than the
// one on screen — an up-facing normal read the cool zenith while the horizon band
// holding the entire sunset was unreachable, and sky_rotation drifted the probe's
// azimuth away from the dome's.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "sky_halfcube.glsl"
#include "atmosphere.glsl"     // procedural Rayleigh+Mie sky (r_sky_proc)

layout(set = 0, binding = 0) uniform samplerCube uSky0;   // weather A
layout(set = 0, binding = 1) uniform samplerCube uSky1;   // weather B
// 2D-array storage view over the cube's 6 layers (mip 0). Writing cube faces as
// an array is the universally-supported path — a CUBE storage view is not.
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2DArray uOut;  // spec cube mip 0 (6 layers)

layout(push_constant) uniform Push {
    vec4 p;    // x = cross-fade weight (A->B), y = face size (px), z = sky_rotation (rad), w unused
    vec4 p2;   // rgb = sky_color (weather tint), w unused
    vec4 p3;   // xyz = direction TO the sun (unit), w unused
    vec4 p4;   // PROCEDURAL SKY: x = enable, y = intensity, z = turbidity, w = Mie g
} pc;

// Cube face (Vulkan layer order: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z) + in-face uv -> dir.
vec3 faceDir(int face, vec2 uv)
{
    vec2 c = uv * 2.0 - 1.0;   // [-1,1]
    if (face == 0) return normalize(vec3( 1.0, -c.y, -c.x));
    if (face == 1) return normalize(vec3(-1.0, -c.y,  c.x));
    if (face == 2) return normalize(vec3( c.x,  1.0,  c.y));
    if (face == 3) return normalize(vec3( c.x, -1.0, -c.y));
    if (face == 4) return normalize(vec3( c.x, -c.y,  1.0));
    return               normalize(vec3(-c.x, -c.y, -1.0));   // 5 = -Z
}

void main()
{
    ivec3 gid  = ivec3(gl_GlobalInvocationID);
    int   size = int(pc.p.y);
    if (gid.x >= size || gid.y >= size) return;

    vec2 uv  = (vec2(gid.xy) + 0.5) / float(size);
    vec3 dir = faceDir(gid.z, uv);                       // WORLD direction of this texel

    // PROCEDURAL SKY: build the probe from the SAME function the dome draw uses, so
    // the light the world receives is by construction the sky the player sees. The
    // sun's own disk is deliberately NOT added here — it is already accounted for as
    // a directional light, and baking it into the probe would double-count it. The
    // Mie aureole around it IS included, because that is genuinely scattered sky.
    if (pc.p4.x > 0.5) {
        vec3 rad = AtmosphereRadiance(dir, normalize(pc.p3.xyz), pc.p4.y, pc.p4.w, pc.p4.z, 0.0);
        imageStore(uOut, gid, vec4(rad, 1.0));
        return;
    }

    // ...resolved to the cube texel the dome would show in that direction.
    vec3 sdir = SkySampleDir(dir, pc.p.z);

    float xf = clamp(pc.p.x, 0.0, 1.0);
    vec3  a  = textureLod(uSky0, sdir, 0.0).rgb;
    vec3  col = (xf > 0.01) ? mix(a, textureLod(uSky1, sdir, 0.0).rgb, xf) : a;

    // The weather tint, exactly as the dome draw applies it (sky.frag.glsl: the R4
    // skybox.vs pre-scale by 1.7). WITHOUT this the probe integrates the RAW cube
    // texture — an unmodulated, essentially daytime-bright blue dome — so the light
    // the ground receives never tracks time of day at all. Measured at a late sunset
    // (r_sky_sh_debug): L0 = (1.24, 1.22, 1.46), a bright blue dome, while the sky on
    // screen was dark and warm. That mismatch is the "terrain is lit by the sky, but
    // somehow uniformly / not right" symptom.
    //
    // Applied RAW (not linearised) on purpose: the dome draw applies it raw too, and
    // probe-vs-dome agreement matters more here than which space the tint nominally
    // lives in. If the linear-colour arc ever converts one, it must convert both.
    col *= pc.p2.rgb * 1.7;

    imageStore(uOut, gid, vec4(col, 1.0));
}
