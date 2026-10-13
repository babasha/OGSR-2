// xrRenderVulkan - shadow sampling MATHS with no descriptor-set dependencies.
//
// Split out of shadow_common.glsl so stages that own a private descriptor set
// (vol_inject.comp reads its own `Vol` UBO + set-0 cascades, not the shared
// Lighting UBO) can share the same code instead of copying it. Everything here
// is a pure function of its arguments — no globals, no bindings — so it is safe
// to include from ANY stage at ANY set layout.
//
// Anything that touches `L` or a named sampler belongs in shadow_common.glsl.
#ifndef SHADOW_MATH_GLSL
#define SHADOW_MATH_GLSL

// Perspective depth -> LINEAR eye depth for the spot/point projections. The
// compare must happen in linear metres: a constant NDC bias is worth centimetres
// near the lamp but METRES near the far plane (light leaked through fences a few
// metres past a parked headlight). n = ComputeSpotVPFor's near plane.
float spotLinZ(float zndc, float f)
{
    const float n = 0.5;
    return n * f / max(f - zndc * (f - n), 1e-4);
}

// Bilinear-weighted PCF tap on a manual-compare map: textureGather fetches the
// 2x2 quad, each texel is COMPARED, then the binary results blend with the
// bilinear weights -> smooth gradient with no texel stair-stepping.
float cascTap(sampler2D smap, vec2 uv, float ref)
{
    vec2 sz = vec2(textureSize(smap, 0));
    vec2 t  = uv * sz - 0.5;
    vec2 f  = fract(t);
    vec4 d  = textureGather(smap, (floor(t) + 1.0) / sz, 0);  // w=(0,0) z=(1,0) x=(0,1) y=(1,1)
    vec4 c  = step(vec4(ref), d);                             // 1 = lit
    return mix(mix(c.w, c.z, f.x), mix(c.x, c.y, f.x), f.y);
}

#endif // SHADOW_MATH_GLSL
