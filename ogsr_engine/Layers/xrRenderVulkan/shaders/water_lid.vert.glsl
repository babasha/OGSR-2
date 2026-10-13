#version 450
// xrRenderVulkan — WATER LID MAP, vertex stage.
//
// Rasterizes the level's opaque statics into the ripple tile to answer the one
// question that decides whether water is VISIBLE: is anything lying directly on
// top of it? The engine's "ground height" map cannot answer it indoors — from
// straight above, the nearest surface over a cellar is the BUILDING'S ROOF, so
// every sheet down there looked wide open and the whole floor came out as one
// body of water (the pool audit drew it: a solid 20 x 13 m slab, not puddles).
//
// The model matrix is pushed per draw. Restricting this to identity-transform
// statics — as the water mask can afford to, since level water always is one —
// left the map EMPTY: the buildings whose floors do the hiding are mesh-union
// instances with real transforms.
layout(location = 0) in vec3 inPos;

layout(push_constant) uniform PC {
    vec4 tile;    // xy = tile origin (world XZ), z = 1 / tile size (m), w = unused
    mat4 model;   // object -> world
} pc;

layout(location = 0) out vec3 vWorld;

void main()
{
    vec3 wp     = (pc.model * vec4(inPos, 1.0)).xyz;
    vec2 uv     = (wp.xz - pc.tile.xy) * pc.tile.z;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vWorld      = wp;
}
