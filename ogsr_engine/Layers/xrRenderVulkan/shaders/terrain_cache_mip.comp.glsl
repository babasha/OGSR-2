#version 450
// TERRAIN COMPOSITE CACHE, cone bake pass 1/2 (r_terra_cone): max-reduce the
// freshly baked composite height (2048, .r of the RG16F cache) into a small
// max-pyramid (1024/512/256). The cone pass then ring-searches this pyramid
// for a conservative occluder bound instead of touching every texel.
// Dispatched once per level (push .x = dst level), barriers between levels.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 0, rgba16f) uniform readonly image2D imgHeight;  // full-res cache (.r = height)
layout(set = 1, binding = 2) uniform sampler2D uPyr;                     // the pyramid itself (texelFetch, prev level)
layout(set = 1, binding = 3, r16f) uniform writeonly image2D outPyr0;
layout(set = 1, binding = 4, r16f) uniform writeonly image2D outPyr1;
layout(set = 1, binding = 5, r16f) uniform writeonly image2D outPyr2;

layout(push_constant) uniform Push { vec4 p0; } pc;   // p0.x = dst pyramid level (0..2)

void main()
{
    int   lvl = int(pc.p0.x + 0.5);
    ivec2 px  = ivec2(gl_GlobalInvocationID.xy);
    ivec2 res = (lvl == 0) ? imageSize(outPyr0) : (lvl == 1) ? imageSize(outPyr1) : imageSize(outPyr2);
    if (px.x >= res.x || px.y >= res.y) return;
    ivec2 s = px * 2;
    float m;
    if (lvl == 0) {
        m = max(max(imageLoad(imgHeight, s).r,              imageLoad(imgHeight, s + ivec2(1, 0)).r),
                max(imageLoad(imgHeight, s + ivec2(0, 1)).r, imageLoad(imgHeight, s + ivec2(1, 1)).r));
        imageStore(outPyr0, px, vec4(m));
    } else {
        int sl = lvl - 1;
        m = max(max(texelFetch(uPyr, s, sl).r,              texelFetch(uPyr, s + ivec2(1, 0), sl).r),
                max(texelFetch(uPyr, s + ivec2(0, 1), sl).r, texelFetch(uPyr, s + ivec2(1, 1), sl).r));
        if (lvl == 1) imageStore(outPyr1, px, vec4(m));
        else          imageStore(outPyr2, px, vec4(m));
    }
}
