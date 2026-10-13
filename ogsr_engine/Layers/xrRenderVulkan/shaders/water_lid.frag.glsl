#version 450
// xrRenderVulkan — WATER LID MAP, fragment stage.
//
// Keeps, per tile texel, the height of the LOWEST solid surface that sits ABOVE
// the water there (MIN blend; cleared to a huge value = open). The sim then has
// the test it actually needed:
//
//   lid - water < ~35 cm  ->  this sheet runs under a floor: invisible, and no
//                             wave may travel through it
//   otherwise             ->  real, visible water
//
// Geometry BELOW the surface is the bottom of the pool, not a lid, so it is
// discarded — otherwise every pond would mask itself off with its own bed.
layout(set = 0, binding = 0) uniform sampler2D uWaterMask;   // water surface height per texel

layout(push_constant) uniform PC {
    vec4 tile;
    mat4 model;
} pc;

layout(location = 0) in  vec3  vWorld;
layout(location = 0) out float outY;

void main()
{
    vec2  uv = (vWorld.xz - pc.tile.xy) * pc.tile.z;
    float wy = textureLod(uWaterMask, uv, 0.0).r;
    if (wy < -9000.0) discard;             // no water under this texel at all
    if (vWorld.y <= wy + 0.02) discard;    // the bottom, or the surface itself
    outY = vWorld.y;
}
