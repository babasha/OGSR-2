#include "stdafx.h" // the converter (_tools/switch_pack/nxa_conv.cpp) builds a copy next to its own stand-in

#include "SoundRender_Adpcm.h"

#include <cmath>

// See SoundRender_Adpcm.h. The decoder is the DSP's arithmetic (as the console runs it: nibble << scale in Q11,
// prediction c0*h0 + c1*h1, round half up, saturate); the encoder picks the eight predictors by Lloyd iterations
// on per-frame least squares, then for every frame the predictor and scale with the least error -- simulated with
// the decoder's own feedback, so what is chosen is what will be heard.

namespace nxa
{
namespace
{
inline s16 sat16(int v) { return s16(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

inline int decode_one(int nibble, int scale, int c0, int c1, int h0, int h1)
{
    const int v = ((nibble * (2048 << scale)) + c0 * h0 + c1 * h1 + 1024) >> 11;
    return sat16(v);
}

// second-order least squares statistics of one frame (x[n] against x[n-1], x[n-2])
struct FrameStats
{
    double r0, r1, r2, a11, a12, a22;
    void add(const FrameStats& o)
    {
        r0 += o.r0; r1 += o.r1; r2 += o.r2;
        a11 += o.a11; a12 += o.a12; a22 += o.a22;
    }
    // prediction error energy with (p1, p2)
    double err(double p1, double p2) const { return r0 - 2.0 * (p1 * r1 + p2 * r2) + p1 * p1 * a11 + 2.0 * p1 * p2 * a12 + p2 * p2 * a22; }
};

void solve(const FrameStats& s, double& p1, double& p2)
{
    const double det = s.a11 * s.a22 - s.a12 * s.a12;
    if (std::fabs(det) > 1e-9 * (s.a11 * s.a22 + 1.0))
    {
        p1 = (s.r1 * s.a22 - s.r2 * s.a12) / det;
        p2 = (s.r2 * s.a11 - s.r1 * s.a12) / det;
    }
    else
    {
        p1 = s.a11 > 0.0 ? s.r1 / s.a11 : 0.0;
        p2 = 0.0;
    }
    // keep the predictor stable (poles inside the circle) with a margin
    if (p2 > 0.98) p2 = 0.98;
    if (p2 < -0.98) p2 = -0.98;
    const double lim = 0.995 - p2;
    if (p1 > lim) p1 = lim;
    if (p1 < -lim) p1 = -lim;
}
} // namespace

void encode(const s16* in, u32 in_stride, u32 count, s16 coefs_out[16], u8* out)
{
    const u32 frames = frames_for(count);
    auto x = [&](s64 i) -> double { return (i < 0 || i >= s64(count)) ? 0.0 : double(in[size_t(i) * in_stride]); };

    // 1. per-frame statistics
    xr_vector<FrameStats> st(frames);
    for (u32 f = 0; f < frames; ++f)
    {
        FrameStats s{};
        for (u32 n = 0; n < samples_per_frame; ++n)
        {
            const s64 i = s64(f) * samples_per_frame + n;
            const double x0 = x(i), x1 = x(i - 1), x2 = x(i - 2);
            s.r0 += x0 * x0; s.r1 += x0 * x1; s.r2 += x0 * x2;
            s.a11 += x1 * x1; s.a12 += x1 * x2; s.a22 += x2 * x2;
        }
        st[f] = s;
    }

    // 2. eight predictors: Lloyd iterations, the error itself as the distance
    double p[8][2] = {{0, 0}, {1.0, 0}, {1.9, -0.92}, {1.6, -0.7}, {1.2, -0.4}, {0.5, 0}, {1.8, -0.82}, {-0.5, 0}};
    xr_vector<u8> cls(frames, 0);
    for (int iter = 0; iter < 24; ++iter)
    {
        FrameStats sum[8]{};
        u32 used[8]{};
        double worst = -1.0;
        u32 worst_f = 0;
        for (u32 f = 0; f < frames; ++f)
        {
            if (st[f].r0 <= 0.0)
                continue;
            double best = 1e300;
            u8 bk = 0;
            for (u8 k = 0; k < 8; ++k)
            {
                const double e = st[f].err(p[k][0], p[k][1]);
                if (e < best)
                    best = e, bk = k;
            }
            cls[f] = bk;
            sum[bk].add(st[f]);
            used[bk]++;
            if (best > worst)
                worst = best, worst_f = f;
        }
        for (int k = 0; k < 8; ++k)
        {
            if (used[k])
                solve(sum[k], p[k][0], p[k][1]);
            else if (worst > 0.0)
            {
                solve(st[worst_f], p[k][0], p[k][1]); // an empty predictor takes the worst-served frame
                worst = -1.0;
            }
        }
    }
    for (int k = 0; k < 8; ++k)
    {
        coefs_out[k * 2 + 0] = sat16(int(std::lround(p[k][0] * 2048.0)));
        coefs_out[k * 2 + 1] = sat16(int(std::lround(p[k][1] * 2048.0)));
    }

    // 3. frames: every predictor, the scale its residual asks for and one above, with the decoder's feedback
    int h0 = 0, h1 = 0;
    for (u32 f = 0; f < frames; ++f)
    {
        int xs[samples_per_frame];
        for (u32 n = 0; n < samples_per_frame; ++n)
            xs[n] = int(x(s64(f) * samples_per_frame + n));

        double best_err = 1e300;
        int best_k = 0, best_s = 0, best_h0 = 0, best_h1 = 0;
        int best_nib[samples_per_frame]{};
        for (int k = 0; k < 8; ++k)
        {
            const int c0 = coefs_out[k * 2], c1 = coefs_out[k * 2 + 1];
            // residual against the true signal (the first two against the decoded history)
            int maxr = 0;
            {
                int y1 = h0, y2 = h1;
                for (u32 n = 0; n < samples_per_frame; ++n)
                {
                    const int pred = (c0 * y1 + c1 * y2 + 1024) >> 11;
                    maxr = _max(maxr, std::abs(xs[n] - pred));
                    y2 = y1;
                    y1 = xs[n];
                }
            }
            int s0 = 0;
            while (s0 < 12 && (7 << s0) < maxr)
                ++s0;
            for (int s = _max(0, s0 - 1); s <= _min(12, s0 + 1); ++s)
            {
                int y0 = h0, y1 = h1, nib[samples_per_frame];
                double e = 0.0;
                const double step = double(2048 << s);
                for (u32 n = 0; n < samples_per_frame; ++n)
                {
                    const int pred = c0 * y0 + c1 * y1;
                    int q = int(std::lround((double(xs[n]) * 2048.0 - double(pred)) / step));
                    q = q < -8 ? -8 : (q > 7 ? 7 : q);
                    const int d = decode_one(q, s, c0, c1, y0, y1);
                    nib[n] = q;
                    const double diff = double(xs[n] - d);
                    e += diff * diff;
                    y1 = y0;
                    y0 = d;
                    if (e >= best_err)
                        break;
                }
                if (e < best_err)
                {
                    best_err = e;
                    best_k = k, best_s = s, best_h0 = y0, best_h1 = y1;
                    memcpy(best_nib, nib, sizeof nib);
                }
            }
        }
        u8* o = out + f * bytes_per_frame;
        o[0] = u8((best_k << 4) | best_s);
        for (u32 n = 0; n < samples_per_frame; n += 2)
            o[1 + n / 2] = u8(((best_nib[n] & 0xF) << 4) | (best_nib[n + 1] & 0xF));
        h0 = best_h0;
        h1 = best_h1;
    }
}

void decode(const u8* data, const s16 coefs[16], u32 total, u32 start, u32 count, s16* out, u32 out_stride)
{
    if (start >= total)
    {
        for (u32 i = 0; i < count; ++i)
            out[size_t(i) * out_stride] = 0;
        return;
    }
    const u32 first_frame = start / samples_per_frame;
    const u32 warm = _min(first_frame, 4u); // frames decoded and thrown away to settle the history
    u32 f = first_frame - warm;
    u32 pos = f * samples_per_frame; // sample index of the frame being decoded
    const u32 end = _min(total, start + count);
    int h0 = 0, h1 = 0;
    u32 written = 0;
    while (pos < end)
    {
        const u8* fr = data + size_t(f) * bytes_per_frame;
        const int k = fr[0] >> 4, s = fr[0] & 0xF;
        const int c0 = coefs[(k & 7) * 2], c1 = coefs[(k & 7) * 2 + 1];
        for (u32 n = 0; n < samples_per_frame && pos < end; ++n, ++pos)
        {
            const u8 b = fr[1 + n / 2];
            int nib = (n & 1) ? (b & 0xF) : (b >> 4);
            nib = (nib ^ 8) - 8; // sign-extend
            const int d = decode_one(nib, s, c0, c1, h0, h1);
            h1 = h0;
            h0 = d;
            if (pos >= start)
                out[size_t(written++) * out_stride] = s16(d);
        }
        ++f;
    }
    for (; written < count; ++written)
        out[size_t(written) * out_stride] = 0;
}
} // namespace nxa
