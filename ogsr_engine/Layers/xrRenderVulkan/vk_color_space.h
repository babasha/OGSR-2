#pragma once

// -----------------------------------------------------------------------------
// CPU-side colour-space conversion for the linear pipeline (r_linear_color).
// -----------------------------------------------------------------------------
// Textures get linearised for free by the sampler once they are loaded as _SRGB
// (see TexColorSpace in vk_texture.h). Everything that reaches the GPU as a
// CONSTANT does not: weather/env colours parsed from .ltx, dynamic light colours
// from item configs and ALife spawn data, particle colours from the effect
// definitions. Those are all authored by eye against a display, i.e. sRGB-encoded,
// so they must be decoded here or they become the one gamma-space term left in an
// otherwise linear light equation — which reads as "everything is too bright and
// washed out", the exact failure this whole arc started from.
//
// Deliberately a header of inlines rather than a .cpp: these sit in per-frame UBO
// fills (vk_env_light, vk_volumetrics, the grass/tree pushes) and the conversion
// must not cost a call. There are only a few dozen colours per frame, so the pow()
// is irrelevant next to being able to reason about where the conversion happened.
//
// SCOPE NOTE — what is deliberately NOT converted here:
//  * Scalars that merely look like colour channels (hemi_color.w is an "R2
//    correction" factor, ambient.w is a sky gate, GpuLight::color[3] is a
//    spot/point flag). Only .rgb is ever touched.
//  * Weather keyframe interpolation. CEnvDescriptorMixer::lerp blends the two
//    bracketing keyframes in gamma space and we convert AFTER it, on upload. That
//    keeps the artist-visible behaviour of a weather transition byte-identical to
//    what it has always been; linearising at parse time would silently change the
//    shape of every sunrise in the game.

// Intentionally depends on nothing but the core math types (Fvector*/Fcolor, via
// stdafx) and <cmath>. This header is pulled into TUs with delicate include order —
// vk_ParticleEffect / vk_gpu_particles_translate set up D3D-stub compat preambles
// before their real includes — so it must not drag in the Vulkan headers.
#include <cmath>

extern int ps_r_linear_color;

namespace VK {
namespace ColorSpace {

// Exact sRGB EOTF (the piecewise curve, not the pow(2.2) approximation). Used on
// the CPU because there is no reason to approximate here: this runs a handful of
// times per frame, and the linear segment near black is exactly where the cheap
// approximation is worst — which is where night-time ambient and distant fog live.
inline float SrgbToLinear(float c)
{
    if (c <= 0.04045f) return c * (1.0f / 12.92f);
    return powf((c + 0.055f) * (1.0f / 1.055f), 2.4f);
}

// True when the renderer is running the linear pipeline. Every conversion below is
// a no-op otherwise, so call sites can be written unconditionally and stay honest:
// the gamma path keeps its historical numbers to the bit.
//
// LATCHED once per process (first call happens no earlier than device init, well
// after user.ltx applied): texture formats bake the choice per texture at load,
// so a mid-session cvar flip half-applies — decoded-dark textures without the
// OETF, or streamed-in textures in the other space than the resident ones. That
// exact confusion was field-reported 23-07-2026 («выключил r_linear_color и
// стало вообще супер темно»). The cvar now takes effect on the NEXT game start.
inline bool Active()
{
    static const bool s_latched = ps_r_linear_color != 0;
    return s_latched;
}

// Convert an authored (sRGB) colour in place. No-op in the gamma pipeline.
inline void Linearize(float& r, float& g, float& b)
{
    if (!Active()) return;
    r = SrgbToLinear(r);
    g = SrgbToLinear(g);
    b = SrgbToLinear(b);
}

// 11-10-2026: the same curve from a 1024-step table with linear interpolation, for colours that end up as 8 bits a
// channel anyway (CPU particle sprites: vkCParticleEffect::BuildVertices packs it with color_rgba_f). Three powf a
// particle were ~4 % of Particles_Extract on the Switch main thread; the table's error is ~1e-5, far under 1/255.
// Outside [0, 1] the exact curve.
struct SrgbLut
{
    static constexpr int N = 1024;
    float v[N + 1];
    SrgbLut()
    {
        for (int i = 0; i <= N; ++i)
            v[i] = SrgbToLinear(float(i) / float(N));
    }
};
inline const SrgbLut g_srgbLut;

inline float SrgbToLinearFast(float c)
{
    if (!(c >= 0.f && c <= 1.f))
        return SrgbToLinear(c);
    const float x = c * float(SrgbLut::N);
    const int i = int(x);
    if (i >= SrgbLut::N)
        return g_srgbLut.v[SrgbLut::N];
    const float f = x - float(i);
    return g_srgbLut.v[i] + (g_srgbLut.v[i + 1] - g_srgbLut.v[i]) * f;
}

inline void LinearizeFast(float& r, float& g, float& b)
{
    if (!Active()) return;
    r = SrgbToLinearFast(r);
    g = SrgbToLinearFast(g);
    b = SrgbToLinearFast(b);
}

inline void Linearize(Fvector3& c)   { Linearize(c.x, c.y, c.z); }
inline void Linearize(Fcolor& c)     { Linearize(c.r, c.g, c.b); }   // .a untouched (coverage, not colour)

// Fvector4 whose .w is a scalar rider (hemi "R2 correction", ambient sky gate) —
// convert .xyz only. Named explicitly so a reader cannot mistake it for a 4-channel
// colour conversion.
inline void LinearizeRGB(Fvector4& c) { Linearize(c.x, c.y, c.z); }

// Array form for the raw float[4] UBO fields (LightUBO.sun_color etc.), where [3]
// is padding or a flag. Same rule: touch [0..2] only.
inline void LinearizeRGB(float* c) { Linearize(c[0], c[1], c[2]); }

} // namespace ColorSpace
} // namespace VK
