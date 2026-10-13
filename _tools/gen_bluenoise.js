// ===========================================================================
//  Void-and-cluster blue-noise tile generator (Ulichney 1993), 2 channels.
//  Output: ogsr_engine/Layers/xrRenderVulkan/vk_gather_bluenoise.h
//
//  WHY THIS EXISTS: the final gather draws one cosine ray per pixel. Its two
//  sampling dimensions used to come from interleaved-gradient noise (azimuth)
//  and a plain hash (ELEVATION) - and elevation is the axis that decides "sky
//  or geometry", i.e. the one carrying nearly all the variance. A white hash
//  there leaves LOW-FREQUENCY clumps that survive the denoiser and every
//  temporal filter downstream. Blue noise puts the residual where a 5x5
//  bilateral and TAA/DLSS can actually remove it.
//
//  ONE TILE, TWO CHANNELS, GENERATED SEPARATELY: the two dimensions must not
//  correlate, or the pair prints a structured pattern (the corduroy hatching
//  rtao.frag documents when one noise source drove both).
//
//  THE DOMAIN IS TOROIDAL, and that buys an identity worth knowing: the
//  Gaussian energy summed over ALL pixels is the same constant everywhere, so
//  "tightest cluster of zeros" == "largest void", and Ulichney's phase 3 needs
//  no second energy map - one min-energy rule drives every phase.
//
//  Run: node _tools/gen_bluenoise.js
// ===========================================================================
const fs = require('fs');

const W = 64, H = 64, N = W * H;
const SIGMA = 1.5;
const R = 6;                       // 13x13 footprint, about 4 sigma

// Deterministic PRNG: the tile must be reproducible or an A/B means nothing.
function mulberry32(a) {
    return function () {
        a |= 0; a = (a + 0x6D2B79F5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

// Precomputed kernel offsets + weights (toroidal wrap applied at use).
const K = [];
for (let dy = -R; dy <= R; ++dy)
    for (let dx = -R; dx <= R; ++dx) {
        const d2 = dx * dx + dy * dy;
        K.push([dx, dy, Math.exp(-d2 / (2 * SIGMA * SIGMA))]);
    }

function splat(energy, idx, sign) {
    const x = idx % W, y = (idx / W) | 0;
    for (let i = 0; i < K.length; ++i) {
        const kx = K[i][0], ky = K[i][1], kw = K[i][2];
        const nx = (x + kx + W) % W, ny = (y + ky + H) % H;
        energy[ny * W + nx] += sign * kw;
    }
}

function extreme(energy, pattern, want, findMax) {
    let best = -1, bestV = findMax ? -Infinity : Infinity;
    for (let p = 0; p < N; ++p) {
        if (pattern[p] !== want) continue;
        const v = energy[p];
        if (findMax ? v > bestV : v < bestV) { bestV = v; best = p; }
    }
    return best;
}

function generate(seed) {
    const rnd = mulberry32(seed);
    const pattern = new Uint8Array(N);
    const energy = new Float64Array(N);

    // --- initial pattern: a tenth of the pixels, then tightened -------------
    const M0 = Math.floor(N / 10);
    let placed = 0;
    while (placed < M0) {
        const p = (rnd() * N) | 0;
        if (!pattern[p]) { pattern[p] = 1; splat(energy, p, +1); placed++; }
    }
    for (let guard = 0; guard < N * 4; ++guard) {
        const tight = extreme(energy, pattern, 1, true);
        pattern[tight] = 0; splat(energy, tight, -1);
        const vd = extreme(energy, pattern, 0, false);
        if (vd === tight) { pattern[tight] = 1; splat(energy, tight, +1); break; }
        pattern[vd] = 1; splat(energy, vd, +1);
    }

    const rank = new Int32Array(N).fill(-1);
    const initPattern = pattern.slice();
    const initEnergy = energy.slice();

    // --- phase 1: strip the initial pattern, ranks M0-1 .. 0 ----------------
    for (let r = M0 - 1; r >= 0; --r) {
        const tight = extreme(energy, pattern, 1, true);
        pattern[tight] = 0; splat(energy, tight, -1);
        rank[tight] = r;
    }

    // --- phases 2+3: refill, ranks M0 .. N-1 --------------------------------
    // One rule for both, see the header: on a torus the largest void IS the
    // tightest cluster of zeros, so phase 3 needs no separate criterion.
    pattern.set(initPattern); energy.set(initEnergy);
    for (let r = M0; r < N; ++r) {
        const vd = extreme(energy, pattern, 0, false);
        pattern[vd] = 1; splat(energy, vd, +1);
        rank[vd] = r;
    }

    const out = new Uint8Array(N);
    for (let p = 0; p < N; ++p) out[p] = Math.min(255, Math.floor(((rank[p] + 0.5) / N) * 256));
    return out;
}

// --- spectrum check: blue noise must be QUIET at low radial frequency -------
function radialSpectrum(tile) {
    const re = new Float64Array(N), im = new Float64Array(N);
    let mean = 0; for (let p = 0; p < N; ++p) mean += tile[p] / 255;
    mean /= N;
    for (let v = 0; v < H; ++v)
        for (let u = 0; u < W; ++u) {
            let sr = 0, si = 0;
            for (let y = 0; y < H; ++y)
                for (let x = 0; x < W; ++x) {
                    const a = -2 * Math.PI * ((u * x) / W + (v * y) / H);
                    const s = tile[y * W + x] / 255 - mean;
                    sr += s * Math.cos(a); si += s * Math.sin(a);
                }
            re[v * W + u] = sr; im[v * W + u] = si;
        }
    const bins = new Float64Array(33), cnt = new Float64Array(33);
    for (let v = 0; v < H; ++v)
        for (let u = 0; u < W; ++u) {
            const du = u > W / 2 ? u - W : u, dv = v > H / 2 ? v - H : v;
            const r = Math.round(Math.sqrt(du * du + dv * dv));
            if (r > 32) continue;
            const i = v * W + u;
            bins[r] += (re[i] * re[i] + im[i] * im[i]) / N; cnt[r]++;
        }
    for (let r = 0; r <= 32; ++r) if (cnt[r]) bins[r] /= cnt[r];
    return bins;
}

const ch0 = generate(0x9E3779B9);
const ch1 = generate(0x85EBCA6B);

for (const pair of [['ch0 (azimuth)  ', ch0], ['ch1 (elevation)', ch1]]) {
    const s = radialSpectrum(pair[1]);
    const low = (s[1] + s[2] + s[3] + s[4]) / 4;
    const high = (s[20] + s[24] + s[28] + s[32]) / 4;
    console.log(pair[0] + ': low-freq power ' + low.toFixed(5) +
                ' | high-freq ' + high.toFixed(5) +
                ' | high/low ' + (high / low).toFixed(1) + 'x   (white noise = 1.0x)');
}

// Cross-correlation of the two channels: must be ~0 or the pair is structured.
let cc = 0, m0 = 0, m1 = 0;
for (let p = 0; p < N; ++p) { m0 += ch0[p]; m1 += ch1[p]; }
m0 /= N; m1 /= N;
let s0 = 0, s1 = 0;
for (let p = 0; p < N; ++p) { const a = ch0[p] - m0, b = ch1[p] - m1; cc += a * b; s0 += a * a; s1 += b * b; }
console.log('channel cross-correlation: ' + (cc / Math.sqrt(s0 * s1)).toFixed(4) + '   (want ~0)');

// --- emit the header -------------------------------------------------------
const lines = [];
for (let p = 0; p < N; p += 16) {
    const row = [];
    for (let i = 0; i < 16 && p + i < N; ++i)
        row.push('0x' + ch0[p + i].toString(16).padStart(2, '0') + ',0x' + ch1[p + i].toString(16).padStart(2, '0'));
    lines.push('    ' + row.join(','));
}
const hdr = '#pragma once\n' +
'// ===========================================================================\n' +
'//  GENERATED by _tools/gen_bluenoise.js - DO NOT EDIT BY HAND.\n' +
'//  ' + W + 'x' + H + ' two-channel blue-noise tile (void-and-cluster, sigma ' + SIGMA + '),\n' +
'//  interleaved RG, one byte per channel. Consumed by vk_pass_gather as the\n' +
'//  final gather\'s two sampling dimensions; see gather.comp.glsl for why the\n' +
'//  ELEVATION axis in particular could not stay on a white hash.\n' +
'//  Tiles seamlessly: the generator\'s energy metric wraps toroidally.\n' +
'// ===========================================================================\n' +
'static const unsigned char kGatherBlueNoiseRG[' + (N * 2) + '] = {\n' +
lines.join(',\n') + '\n' +
'};\n' +
'static const unsigned kGatherBlueNoiseDim = ' + W + ';\n';
fs.writeFileSync('D:/OGSR-ONLINE/ogsr_engine/Layers/xrRenderVulkan/vk_gather_bluenoise.h', hdr, 'utf8');
console.log('wrote vk_gather_bluenoise.h (' + hdr.length + ' bytes)');
