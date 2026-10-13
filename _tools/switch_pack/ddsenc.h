// ddsenc.h -- offline DDS transcoder for the Switch data package (used by dbtool at pack-build time).
//
// WHY: the package is built FROM the unmodified install (or from any mod's archives), on the fly, so the original files
// stay untouched and old/other mods keep working. Uncompressed DDS files (a single 4096^2 A8R8G8B8 weapon skin is 64 MB)
// are read in full from squashfs and decompressed on a 1 GHz A57 at ~60 MB/s -- one such texture is a 1 s hitch -- and
// they take 4-8x the VRAM/SD space of the block-compressed equivalent. They are re-encoded to DXT1 (opaque) / DXT5
// (alpha) -- formats every GPU and the stock engine already understand -- with the top mips dropped to the pack's cap.
//
// Encoder: per-block range fit (principal-axis endpoints + 2 least-squares refinement passes), 565 endpoints, 4-colour
// DXT1 mode only (no punch-through alpha). Quality is "good for albedo/normal", not offline-optimal; it is only used on
// files that were never block-compressed in the first place.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ddsenc {

struct Img { uint32_t w = 0, h = 0; std::vector<uint8_t> rgba; };   // 8-bit RGBA, row-major

inline uint16_t pack565(const float c[3])
{
    auto q = [](float v, int bits) { int m = (1 << bits) - 1; int i = (int)std::lround(std::min(std::max(v, 0.f), 255.f) / 255.f * m); return i; };
    return (uint16_t)((q(c[0], 5) << 11) | (q(c[1], 6) << 5) | q(c[2], 5));
}
inline void unpack565(uint16_t v, float c[3])
{
    int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    c[0] = float((r << 3) | (r >> 2)); c[1] = float((g << 2) | (g >> 4)); c[2] = float((b << 3) | (b >> 2));
}

// Encode one 4x4 block of RGB (px[16][3], 0..255) to 8 bytes of DXT1 colour data.
inline void encodeColorBlock(const float px[16][3], uint8_t out[8])
{
    float mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 }, avg[3] = { 0, 0, 0 };
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], px[i][k]); mx[k] = std::max(mx[k], px[i][k]); avg[k] += px[i][k] / 16.f; }
    if (mx[0] - mn[0] < 1.f && mx[1] - mn[1] < 1.f && mx[2] - mn[2] < 1.f)   // flat block
    {
        uint16_t c = pack565(avg);
        memcpy(out, &c, 2); memcpy(out + 2, &c, 2); memset(out + 4, 0, 4);
        return;
    }
    // principal axis by power iteration on the covariance
    float cov[6] = { 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 16; ++i)
    {
        float d[3] = { px[i][0] - avg[0], px[i][1] - avg[1], px[i][2] - avg[2] };
        cov[0] += d[0] * d[0]; cov[1] += d[0] * d[1]; cov[2] += d[0] * d[2];
        cov[3] += d[1] * d[1]; cov[4] += d[1] * d[2]; cov[5] += d[2] * d[2];
    }
    float ax[3] = { mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2] };
    for (int it = 0; it < 8; ++it)
    {
        float t[3] = { cov[0] * ax[0] + cov[1] * ax[1] + cov[2] * ax[2],
                       cov[1] * ax[0] + cov[3] * ax[1] + cov[4] * ax[2],
                       cov[2] * ax[0] + cov[4] * ax[1] + cov[5] * ax[2] };
        float l = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (l < 1e-6f) break;
        ax[0] = t[0] / l; ax[1] = t[1] / l; ax[2] = t[2] / l;
    }
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 16; ++i)
    {
        float t = (px[i][0] - avg[0]) * ax[0] + (px[i][1] - avg[1]) * ax[1] + (px[i][2] - avg[2]) * ax[2];
        lo = std::min(lo, t); hi = std::max(hi, t);
    }
    float c0[3], c1[3];
    for (int k = 0; k < 3; ++k) { c0[k] = avg[k] + ax[k] * hi; c1[k] = avg[k] + ax[k] * lo; }

    uint8_t idx[16];
    uint16_t e0 = 0, e1 = 0;
    for (int pass = 0; pass < 3; ++pass)
    {
        e0 = pack565(c0); e1 = pack565(c1);
        float p0[3], p1[3];
        unpack565(e0, p0); unpack565(e1, p1);
        if (e0 < e1) { std::swap(e0, e1); for (int k = 0; k < 3; ++k) std::swap(p0[k], p1[k]); }   // e0 > e1 -> 4-colour mode
        float pal[4][3];
        for (int k = 0; k < 3; ++k) { pal[0][k] = p0[k]; pal[1][k] = p1[k]; pal[2][k] = (2 * p0[k] + p1[k]) / 3; pal[3][k] = (p0[k] + 2 * p1[k]) / 3; }
        // nearest palette entry per pixel
        for (int i = 0; i < 16; ++i)
        {
            float best = 1e30f; int bi = 0;
            for (int j = 0; j < 4; ++j)
            {
                float d0 = px[i][0] - pal[j][0], d1 = px[i][1] - pal[j][1], d2 = px[i][2] - pal[j][2];
                float d = d0 * d0 + d1 * d1 + d2 * d2;
                if (d < best) { best = d; bi = j; }
            }
            idx[i] = (uint8_t)bi;
        }
        if (pass == 2) break;
        // least squares for the two endpoints given the indices (weights 1, 0, 2/3, 1/3 on e0)
        static const float w0[4] = { 1.f, 0.f, 2.f / 3.f, 1.f / 3.f };
        float aa = 0, bb = 0, ab = 0, r0[3] = { 0, 0, 0 }, r1[3] = { 0, 0, 0 };
        for (int i = 0; i < 16; ++i)
        {
            float a = w0[idx[i]], b = 1.f - a;
            aa += a * a; bb += b * b; ab += a * b;
            for (int k = 0; k < 3; ++k) { r0[k] += a * px[i][k]; r1[k] += b * px[i][k]; }
        }
        float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-6f) break;
        for (int k = 0; k < 3; ++k) { c0[k] = (r0[k] * bb - r1[k] * ab) / det; c1[k] = (r1[k] * aa - r0[k] * ab) / det; }
    }
    if (e0 == e1)   // degenerate after quantisation: force 4-colour mode with everything on index 0
    {
        memcpy(out, &e0, 2); memcpy(out + 2, &e1, 2); memset(out + 4, 0, 4);
        return;
    }
    memcpy(out, &e0, 2); memcpy(out + 2, &e1, 2);
    // DXT1 palette order: 0 = e0, 1 = e1, 2 = 2/3 e0 + 1/3 e1, 3 = 1/3 e0 + 2/3 e1 -- matches idx[] above
    uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= (uint32_t)idx[i] << (2 * i);
    memcpy(out + 4, &bits, 4);
}

// DXT5 alpha block (BC4-style, 8-level mode a0 > a1).
inline void encodeAlphaBlock(const uint8_t a[16], uint8_t out[8])
{
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; ++i) { mn = std::min<int>(mn, a[i]); mx = std::max<int>(mx, a[i]); }
    if (mn == mx) { out[0] = out[1] = (uint8_t)mx; memset(out + 2, 0, 6); return; }   // a0 == a1: index 0 -> a0
    int a0 = mx, a1 = mn;   // a0 > a1: 8 interpolated values
    float pal[8];
    pal[0] = (float)a0; pal[1] = (float)a1;
    for (int i = 1; i <= 6; ++i) pal[1 + i] = ((7 - i) * a0 + i * a1) / 7.f;
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i)
    {
        float best = 1e30f; int bi = 0;
        for (int j = 0; j < 8; ++j) { float d = std::fabs(a[i] - pal[j]); if (d < best) { best = d; bi = j; } }
        bits |= (uint64_t)bi << (3 * i);
    }
    out[0] = (uint8_t)a0; out[1] = (uint8_t)a1;
    for (int i = 0; i < 6; ++i) out[2 + i] = (uint8_t)(bits >> (8 * i));
}

// Whole image -> DXT1 (alpha=false) or DXT5 (alpha=true) block data.
inline void encodeImage(const Img& im, bool alpha, std::vector<uint8_t>& out)
{
    const uint32_t bw = std::max(1u, (im.w + 3) / 4), bh = std::max(1u, (im.h + 3) / 4);
    const size_t bs = alpha ? 16 : 8;
    const size_t base = out.size();
    out.resize(base + (size_t)bw * bh * bs);
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            float px[16][3]; uint8_t av[16];
            for (int i = 0; i < 16; ++i)
            {
                uint32_t x = std::min(bx * 4 + (i & 3), im.w - 1), y = std::min(by * 4 + (i >> 2), im.h - 1);
                const uint8_t* s = &im.rgba[((size_t)y * im.w + x) * 4];
                px[i][0] = s[0]; px[i][1] = s[1]; px[i][2] = s[2]; av[i] = s[3];
            }
            uint8_t* o = &out[base + ((size_t)by * bw + bx) * bs];
            if (alpha) { encodeAlphaBlock(av, o); encodeColorBlock(px, o + 8); }
            else encodeColorBlock(px, o);
        }
}

inline Img boxHalf(const Img& s)
{
    Img d; d.w = std::max(1u, s.w >> 1); d.h = std::max(1u, s.h >> 1); d.rgba.resize((size_t)d.w * d.h * 4);
    for (uint32_t y = 0; y < d.h; ++y)
        for (uint32_t x = 0; x < d.w; ++x)
            for (int k = 0; k < 4; ++k)
            {
                uint32_t x0 = std::min(x * 2, s.w - 1), x1 = std::min(x * 2 + 1, s.w - 1), y0 = std::min(y * 2, s.h - 1), y1 = std::min(y * 2 + 1, s.h - 1);
                int v = s.rgba[((size_t)y0 * s.w + x0) * 4 + k] + s.rgba[((size_t)y0 * s.w + x1) * 4 + k] +
                        s.rgba[((size_t)y1 * s.w + x0) * 4 + k] + s.rgba[((size_t)y1 * s.w + x1) * 4 + k];
                d.rgba[((size_t)y * d.w + x) * 4 + k] = (uint8_t)((v + 2) / 4);
            }
    return d;
}

struct PixFmt { uint32_t bits = 0, rm = 0, gm = 0, bm = 0, am = 0; bool lum = false, alphaOnly = false; };

inline int shiftOf(uint32_t m) { int s = 0; while (m && !(m & 1)) { m >>= 1; ++s; } return s; }
inline int widthOf(uint32_t m) { int w = 0; m >>= shiftOf(m); while (m & 1) { ++w; m >>= 1; } return w; }
inline uint8_t expand(uint32_t v, uint32_t mask)
{
    if (!mask) return 255;
    int w = widthOf(mask); uint32_t x = (v & mask) >> shiftOf(mask);
    if (w >= 8) return (uint8_t)(x >> (w - 8));
    uint32_t maxv = (1u << w) - 1;
    return (uint8_t)((x * 255u + maxv / 2) / maxv);
}

// Reads `levels` mips starting at `data` (file offset 128). Returns false if the layout does not add up.
inline bool decodeUncompressed(const uint8_t* data, size_t avail, uint32_t w, uint32_t h, const PixFmt& f, Img& out, size_t& used)
{
    const uint32_t bpp = f.bits / 8;
    if (!bpp || bpp > 4) return false;
    used = (size_t)w * h * bpp;
    if (used > avail) return false;
    out.w = w; out.h = h; out.rgba.resize((size_t)w * h * 4);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i)
    {
        uint32_t v = 0; memcpy(&v, data + i * bpp, bpp);
        uint8_t* d = &out.rgba[i * 4];
        if (f.lum) { d[0] = d[1] = d[2] = expand(v, f.rm); d[3] = f.am ? expand(v, f.am) : 255; }
        else if (f.alphaOnly) { d[0] = d[1] = d[2] = 255; d[3] = expand(v, f.am); }
        else { d[0] = expand(v, f.rm); d[1] = expand(v, f.gm); d[2] = expand(v, f.bm); d[3] = f.am ? expand(v, f.am) : 255; }
    }
    return true;
}


// Decode DXT1/DXT5 block data back to RGBA (verification only: PSNR of the encoder against its own input).
inline void decodeBlocks(const uint8_t* b, uint32_t w, uint32_t h, bool alpha, Img& out)
{
    out.w = w; out.h = h; out.rgba.assign((size_t)w * h * 4, 255);
    const uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
    const size_t bs = alpha ? 16 : 8;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            const uint8_t* blk = b + ((size_t)by * bw + bx) * bs;
            uint8_t av[8] = {}; uint32_t aidx[16] = {};
            if (alpha)
            {
                av[0] = blk[0]; av[1] = blk[1];
                if (av[0] > av[1]) for (int i = 1; i <= 6; ++i) av[1 + i] = (uint8_t)(((7 - i) * av[0] + i * av[1]) / 7);
                else { for (int i = 1; i <= 4; ++i) av[1 + i] = (uint8_t)(((5 - i) * av[0] + i * av[1]) / 5); av[6] = 0; av[7] = 255; }
                uint64_t bits = 0; for (int i = 0; i < 6; ++i) bits |= (uint64_t)blk[2 + i] << (8 * i);
                for (int i = 0; i < 16; ++i) aidx[i] = (uint32_t)((bits >> (3 * i)) & 7);
                blk += 8;
            }
            uint16_t e0, e1; memcpy(&e0, blk, 2); memcpy(&e1, blk + 2, 2);
            float p0[3], p1[3]; unpack565(e0, p0); unpack565(e1, p1);
            float pal[4][3];
            for (int k = 0; k < 3; ++k)
            {
                pal[0][k] = p0[k]; pal[1][k] = p1[k];
                if (e0 > e1 || alpha) { pal[2][k] = (2 * p0[k] + p1[k]) / 3; pal[3][k] = (p0[k] + 2 * p1[k]) / 3; }
                else { pal[2][k] = (p0[k] + p1[k]) / 2; pal[3][k] = 0; }
            }
            uint32_t bits; memcpy(&bits, blk + 4, 4);
            for (int i = 0; i < 16; ++i)
            {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                uint8_t* d = &out.rgba[((size_t)y * w + x) * 4];
                const float* c = pal[(bits >> (2 * i)) & 3];
                d[0] = (uint8_t)std::lround(c[0]); d[1] = (uint8_t)std::lround(c[1]); d[2] = (uint8_t)std::lround(c[2]);
                if (alpha) d[3] = av[aidx[i]];
            }
        }
}

inline double psnr(const Img& a, const Img& b, bool withAlpha)
{
    double se = 0; size_t n = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
        for (int k = 0; k < (withAlpha ? 4 : 3); ++k) { double d = double(a.rgba[i + k]) - b.rgba[i + k]; se += d * d; ++n; }
    if (se <= 0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / (se / n));
}


// DXT3/DXT5 whose alpha is 255 in EVERY texel of EVERY mip (artists save plain albedo as DXT5 all the time) carry 8 bytes
// per block of nothing. Rewrite them as DXT1: copy the colour half of each block. Lossless. A DXT5 colour block is always
// decoded in 4-colour mode, a DXT1 one only when c0 > c1 (c0 <= c1 selects 3-colour + transparent), so blocks with
// c0 < c1 get their endpoints swapped and indices remapped (0<->1, 2<->3), and c0 == c1 blocks (one colour) use index 0.
// Plain DDS only: no cube/volume/DX10. Returns true if `d` was rewritten.
inline bool dropOpaqueAlpha(std::vector<uint8_t>& d)
{
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) return false;
    auto rd = [&](size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; };
    if (!(rd(80) & 0x4) || rd(112) & (0x200 | 0x200000)) return false;
    const bool dxt5 = !memcmp(&d[84], "DXT5", 4), dxt3 = !memcmp(&d[84], "DXT3", 4);
    if (!dxt5 && !dxt3) return false;
    const uint32_t w = rd(16), h = rd(12), mips = rd(28) ? rd(28) : 1;
    // pass 1: is every alpha texel 255?
    size_t blocks = 0;
    { uint32_t lw = w, lh = h; for (uint32_t i = 0; i < mips; ++i) { blocks += (size_t)std::max(1u, (lw + 3) / 4) * std::max(1u, (lh + 3) / 4); lw = std::max(1u, lw >> 1); lh = std::max(1u, lh >> 1); } }
    if (128 + blocks * 16 > d.size()) return false;
    for (size_t b = 0; b < blocks; ++b)
    {
        const uint8_t* blk = &d[128 + b * 16];
        if (dxt3) { for (int i = 0; i < 8; ++i) if (blk[i] != 0xFF) return false; continue; }
        uint8_t av[8]; av[0] = blk[0]; av[1] = blk[1];
        if (av[0] > av[1]) for (int i = 1; i <= 6; ++i) av[1 + i] = (uint8_t)(((7 - i) * av[0] + i * av[1]) / 7);
        else { for (int i = 1; i <= 4; ++i) av[1 + i] = (uint8_t)(((5 - i) * av[0] + i * av[1]) / 5); av[6] = 0; av[7] = 255; }
        uint64_t bits = 0; for (int i = 0; i < 6; ++i) bits |= (uint64_t)blk[2 + i] << (8 * i);
        for (int i = 0; i < 16; ++i) if (av[(bits >> (3 * i)) & 7] != 255) return false;
    }
    std::vector<uint8_t> nd(d.begin(), d.begin() + 128);
    nd.reserve(128 + blocks * 8);
    for (size_t b = 0; b < blocks; ++b)
    {
        uint8_t c[8]; memcpy(c, &d[128 + b * 16 + 8], 8);
        uint16_t c0, c1; memcpy(&c0, c, 2); memcpy(&c1, c + 2, 2);
        uint32_t idx; memcpy(&idx, c + 4, 4);
        if (c0 == c1) idx = 0;
        else if (c0 < c1)
        {
            std::swap(c0, c1);
            uint32_t n = 0;
            for (int i = 0; i < 16; ++i) { uint32_t v = (idx >> (2 * i)) & 3; v = (v == 0) ? 1 : (v == 1) ? 0 : (v == 2) ? 3 : 2; n |= v << (2 * i); }
            idx = n;
        }
        memcpy(c, &c0, 2); memcpy(c + 2, &c1, 2); memcpy(c + 4, &idx, 4);
        nd.insert(nd.end(), c, c + 8);
    }
    memcpy(&nd[84], "DXT1", 4);
    uint32_t lin = (uint32_t)((size_t)std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * 8);
    memcpy(&nd[20], &lin, 4);
    d.swap(nd);
    return true;
}

}   // namespace ddsenc
