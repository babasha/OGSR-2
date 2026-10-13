// xrRenderVulkan — TESSENDORF FFT OCEAN: constants and complex arithmetic shared
// by the four compute stages (h0 → spectrum → FFT → assemble).
//
// WHY THIS EXISTS AT ALL, given there is already a nine-octave analytic swell in
// water_common.glsl: that swell is a sum of `exp(sin())` lobes displaced along Y
// only. A sum of vertically-displaced sines cannot produce a SHARP CREST — the
// peak of a sine is round by construction, and no amount of octaves changes it.
// Real wind waves are sharp on top and flat in the trough because the water at
// the crest is also displaced HORIZONTALLY, inward from both sides. That is the
// whole of Tessendorf's "choppy wave" term, it needs the field in the Fourier
// domain to compute, and it is the single thing the analytic model cannot fake.
//
// The spectrum is Phillips (Tessendorf 1999, "Simulating Ocean Water"); the same
// mathematics as the reference project this was modelled on
// (github.com/kentril0/WaterSurfaceRendering, MIT). That project computes its FFT
// on the CPU with FFTW3 + OpenMP and uploads the result every frame. Here it is
// all compute shaders: no third-party dependency, no per-frame upload, and no
// cost on the game thread — which a renderer that also has to run a headless
// server cares about rather a lot.
#ifndef WATER_FFT_COMMON_GLSL
#define WATER_FFT_COMMON_GLSL

// Tile resolution. 256 keeps one whole row in shared memory (256 × vec2 = 2 KB),
// which is what lets a single workgroup run all eight butterfly stages without
// ever going back to memory.
#define WFFT_N       256
#define WFFT_LOG2N   8
#define WFFT_MASK    255
// Three CASCADES, each its own periodic tile. One tile alone is a liar: at any
// size big enough to hold a 200 m swell, its 256 samples are 78 cm apart and
// every wave shorter than a couple of metres is gone; at any size fine enough to
// resolve chop, the tile is small enough that the eye reads the repeat. Three
// tiles at very different sizes, each carrying only the band it can actually
// resolve, has neither problem — and their periods do not align, so the combined
// field has no visible repeat at all.
#define WFFT_CASCADES 3

const float WFFT_G   = 9.81;
const float WFFT_TAU = 6.28318530718;

vec2 cMul(vec2 a, vec2 b) { return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
vec2 cConj(vec2 a)        { return vec2(a.x, -a.y); }
vec2 cExp(float t)        { return vec2(cos(t), sin(t)); }

// Tile size of cascade `c`, in metres.
float wfftLen(float base, float ratio, int c) { return base * pow(ratio, float(c)); }

// BAND LIMITS. Cascade c carries wavelengths from its own tile size (its
// fundamental — nothing longer fits) down to the NEXT cascade's tile size, at
// which point the next cascade takes over. Without this every cascade would
// contain every wavelength it can represent, the three would sum to three times
// the energy, and the smallest tile's repeat would be visible in the sum because
// it would be carrying long waves it has no business carrying.
float wfftKMin(float base, float ratio, int c)
{
    return WFFT_TAU / wfftLen(base, ratio, c);
}
float wfftKMax(float base, float ratio, int c, int cascades)
{
    if (c + 1 >= cascades) return 1e9;               // the finest cascade runs to its Nyquist
    return WFFT_TAU / wfftLen(base, ratio, c + 1);
}

#endif
