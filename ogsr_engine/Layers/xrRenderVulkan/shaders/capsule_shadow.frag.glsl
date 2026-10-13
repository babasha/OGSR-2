#version 450
// xrRenderVulkan — analytic capsule sun shadows for characters (r_shadow_lite 1). See vk_pass_capsule_shadow.h.
//
// One fullscreen triangle (shares sunshafts.vert), up to five capsules per draw. For every pixel the world point is
// rebuilt from the scene depth, then the ray from that point TOWARD THE SUN is tested against each capsule (closest
// approach between a ray and a segment, Ericson 5.1.9). Inside the capsule radius = shadowed; the edge softens with the
// distance the ray travels before it meets the caster (a real penumbra). The caster's own pixels are excluded by a ramp
// on that distance. A soft contact patch is added on the ground around the feet.
//
// Output is a multiply: blend dst * (1 - a), colour ignored.

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uDepth;      // scene depth (D32)

layout(push_constant) uniform PC {
    vec4 cap[10];     // capsule i: cap[2i] = (A.xyz, radius), cap[2i+1] = (B.xyz, strength; <= 0 = unused)
    vec4 camPos;      // xyz = camera position,      w = proj _33
    vec4 camDir;      // xyz = camera forward,       w = proj _43
    vec4 camRightT;   // xyz = right * tan(fovX/2),  w = unused
    vec4 camTopT;     // xyz = up * tan(fovY/2),     w = unused
    vec4 sun;         // xyz = unit vector TO the sun, w = overall darkening (strength x sun elevation x sun share of the light)
} pc;

void main()
{
    float zndc = texture(uDepth, vUV).r;
    if (zndc >= 0.99999) discard;                                  // sky

    float zview = clamp(pc.camDir.w / (zndc - pc.camPos.w), 0.0, 10000.0);
    vec2  ndcXY = vec2(vUV.x * 2.0 - 1.0, 1.0 - 2.0 * vUV.y);       // D3D ndc (y up)
    vec3  ray   = pc.camDir.xyz + pc.camRightT.xyz * ndcXY.x + pc.camTopT.xyz * ndcXY.y;
    vec3  p     = pc.camPos.xyz + ray * zview;

    const vec3 Ls = pc.sun.xyz;
    float occ = 0.0;

    for (int i = 0; i < 5; ++i) {
        vec4 a = pc.cap[2 * i];
        vec4 b = pc.cap[2 * i + 1];
        if (b.w <= 0.0) continue;

        vec3  A  = a.xyz;
        float r  = a.w;
        vec3  d2 = b.xyz - A;                  // capsule axis (A -> B)
        vec3  w  = p - A;
        float c  = max(dot(d2, d2), 1e-4);
        float bb = dot(Ls, d2);
        float d  = dot(Ls, w);
        float e  = dot(d2, w);

        // closest approach of the ray p + s*Ls (s >= 0) to the segment A + t*d2 (t in 0..1)
        float denom = c - bb * bb;
        float s = (denom > 1e-4) ? (bb * e - c * d) / denom : 0.0;
        float t = clamp((bb * s + e) / c, 0.0, 1.0);
        s = clamp(bb * t - d, 0.0, 40.0);
        t = clamp((bb * s + e) / c, 0.0, 1.0);
        float dist = length((p + Ls * s) - (A + d2 * t));

        // penumbra widens with the distance the light travels between receiver and caster
        float pen   = r * 0.35 + s * 0.12;
        float shd  = 1.0 - smoothstep(r * 0.6, r + pen, dist);
        // the character's own pixels sit at s ~ 0: no self-shadow, and no shadow ON the contact point itself
        shd *= smoothstep(r, r * 2.2 + 0.25, s);

        // contact patch on the ground around the feet (horizontal falloff from the axis, only near the feet)
        float tc    = clamp(dot(p - A, d2) / c, 0.0, 1.0);
        float dd    = length(p - (A + d2 * tc));
        float band  = 1.0 - smoothstep(0.10, 0.55, p.y - (A.y - r));
        float cont  = (1.0 - smoothstep(r * 0.8, r * 2.6, dd)) * band * smoothstep(r * 1.05, r * 1.35, dd) * 0.45;

        occ = max(occ, max(shd, cont) * b.w);
    }

    float al = clamp(occ * pc.sun.w, 0.0, 0.85);
    if (al < 0.003) discard;
    outColor = vec4(0.0, 0.0, 0.0, al);
}
