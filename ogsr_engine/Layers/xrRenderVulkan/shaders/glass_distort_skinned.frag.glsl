#version 450
// xrRenderVulkan — GLASS refraction, SKINNED variant (kinematics furniture/door
// panes: the village cabinet etc.). Paired with the regular skinned.vert (GPU
// skinning); writes the same procedural "uneven old glass" wobble into the
// distortion RT as glass_distort.frag. Push block mirrors SkinPush — the free
// `hemi` slot carries the refraction strength (r_glass_refr) for this draw.
layout(push_constant) uniform PC {
    mat4  mvp;
    uint  skinMode;
    uint  baseBone;
    uint  boneCount;
    float hudMode;
    float hemi;      // = r_glass_refr strength for the distort draw
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;

void main()
{
    vec2 w = vec2(sin(v_uv.x * 37.0 + v_uv.y * 11.0), cos(v_uv.y * 29.0 - v_uv.x * 13.0))
           + 0.5 * vec2(sin(v_uv.y * 113.0 + v_uv.x * 41.0), cos(v_uv.x * 97.0));
    o = vec4(0.5 + w * 0.04 * pc.hemi, 0.5, 0.85);
}
