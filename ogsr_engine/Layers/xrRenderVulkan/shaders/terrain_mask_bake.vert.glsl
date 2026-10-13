#version 450
// Terrain splat-mask BAKE (mask-less maps, VK::TerrainMask): draw each terrain
// region's geometry top-down into the mask RT; the FS fills the region's one-hot
// channel. Sampled bilinearly at low res afterwards -> soft region transitions
// the level author never painted.
layout(push_constant) uniform Push {
    vec4 rect;    // ox, oz, 1/sizeX, 1/sizeZ  (world XZ -> mask UV)
    vec4 color;   // this region's one-hot splat channel
} pc;
layout(location = 0) in vec3 aPos;
void main()
{
    vec2 uv = (aPos.xz - pc.rect.xy) * pc.rect.zw;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
}
