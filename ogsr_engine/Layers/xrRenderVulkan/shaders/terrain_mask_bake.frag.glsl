#version 450
// Terrain splat-mask bake FS — flat one-hot region color (see the .vert).
layout(push_constant) uniform Push {
    vec4 rect;
    vec4 color;
} pc;
layout(location = 0) out vec4 outColor;
void main() { outColor = pc.color; }
