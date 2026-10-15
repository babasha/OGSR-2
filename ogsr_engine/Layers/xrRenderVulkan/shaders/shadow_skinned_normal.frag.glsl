#version 450
// xrRenderVulkan — NORMAL G-buffer fragment for SKINNED dynamics (NPCs).
// Alpha-test MUST match skinned.frag / shadow_skinned_at.frag (a < 0.25) so the
// normals written cover exactly the same pixels the depth prepass did — hair/
// strap cutouts must NOT stamp a bogus normal onto the background behind them.
// Output: world-space normal *0.5+0.5 in rgb, a=1 = "this pixel has a real
// normal" (GTAO uses it; a=0 elsewhere → GTAO falls back to depth recon).

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vNormal;

layout(set = 1, binding = 0) uniform sampler2D uTexDiffuse;

layout(location = 0) out vec4 outNormal;

void main()
{
    if (texture(uTexDiffuse, vUV).a < 0.25)
        discard;
    vec3 n = normalize(vNormal);
    outNormal = vec4(n * 0.5 + 0.5, 1.0);
}
