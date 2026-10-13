#version 450
// xrRenderVulkan — WATER POOL MASK, vertex stage.
//
// Rasterizes the water surfaces from straight above into the ripple sim's tile,
// writing the world HEIGHT of the surface. The sim needs it because its wave
// equation otherwise runs on one unbroken sheet: a splash in one puddle crossed
// the dry floor between them and surfaced in every other puddle in range.
//
// Tile space, not world space: x/z map to the tile's [0,1] box and straight to
// NDC, so texel (0,0) of the mask is texel (0,0) of the sim. No depth buffer —
// overlaps resolve with a MAX blend, which keeps the TOP surface where two
// pools stack (a floor pool under a balcony puddle).
layout(location = 0) in vec3 inPos;

layout(push_constant) uniform PC {
    vec4 tile;    // xy = tile origin (world XZ), z = 1 / tile size (m), w = STRICT
    mat4 rainVP;  // straight-down ortho of the ground-height map (fragment stage)
    // x = this body's fetch (m), y = metres per unit of ortho depth,
    // z = how far above the still line the ground map is still trusted (m),
    // w = 1 while the second target (live surface + ground) is attached.
    vec4 basin;
} pc;

layout(location = 0) out vec3 vWorld;

void main()
{
    vec2 uv     = (inPos.xz - pc.tile.xy) * pc.tile.z;   // 0..1 across the tile
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vWorld      = inPos;
}
