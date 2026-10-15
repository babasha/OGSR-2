// xrRenderVulkan — shared-exponent RGB9E5 pack/unpack, ONE copy.
//
// Same contract as common_math.glsl: pure functions of their arguments, no
// descriptor dependencies, any stage. Split into its OWN header (rather than
// common_math) on purpose: common_math is included by shaders whose committed
// .spv must not move when nothing they use changed — glslc without -O emits
// unused function bodies, so adding helpers to a shared header shifts every
// consumer's binary. This header is included ONLY where the packing is used.
//
// The format is the standard E5B9G9R9 shared-exponent layout: three 9-bit
// mantissas under one 5-bit exponent (bias 15), positive-only, ~[0, 65408].
// Born for what it carries here — HDR radiance that fits in ONE uint: the
// grass instance record has exactly one spare float (color.g, freed when the
// trail press-down died), and the per-tuft probe-field sample rides in it as
// these bits (detail_generate.comp packs, detail.frag GRASS_GI_INST unpacks).
// 9-bit mantissas are plenty for an ambient term that is then multiplied by
// albedo and tonemapped; the shared exponent keeps dusk (~1e-2) and noon
// (~1e1) in the same carrier without banding.
#ifndef RGB9E5_GLSL
#define RGB9E5_GLSL

uint packRGB9E5(vec3 c)
{
    const float kMax = 65408.0;                    // (511/512) * 2^(31-15-9+9) — format ceiling
    c = clamp(c, vec3(0.0), vec3(kMax));
    const float m = max(c.r, max(c.g, c.b));
    if (m < 6.1e-5) return 0u;                     // below the smallest representable step
    // Shared exponent: mantissa = round(c / 2^(e-15-9)) must land in [0, 511].
    int e = clamp(int(floor(log2(m))) + 16, 1, 31);
    float scale = exp2(float(e - 24));             // e - 15 - 9
    uvec3 q = uvec3(round(c / scale));
    // The max channel can round UP to 512 — bump the exponent once, never twice.
    if (max(q.r, max(q.g, q.b)) > 511u) { e += 1; scale *= 2.0; q = uvec3(round(c / scale)); }
    return (uint(e) << 27) | (q.b << 18) | (q.g << 9) | q.r;
}

vec3 unpackRGB9E5(uint p)
{
    const float scale = exp2(float(int(p >> 27) - 24));
    return vec3(float(p & 0x1FFu), float((p >> 9) & 0x1FFu), float((p >> 18) & 0x1FFu)) * scale;
}

#endif // RGB9E5_GLSL
