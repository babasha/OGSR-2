// Offline reproduction of the flat-floor GTAO "bands" (see vulkan-ssao-gtao memory).
// Mirrors the CURRENT ssao.frag math (exact acos, NB=2 hybrid centered normal,
// min-offset (2+k)/nSample half-res texels, IGN noise, temporal off) against a
// synthetic INFINITE FLAT FLOOR viewed at a grazing angle, with the two real-world
// imperfections the old gtao_selftest.js lacked:
//   1. D32 float32 depth quantization (Math.fround on stored zndc + fp32 zview math)
//   2. NEAREST depth sampling: depth comes from the nearest FULL-res texel center,
//      but the reconstruction ray is built at the QUERY uv (half-res, incl. the
//      odd-height mismatch 1571 vs 2*785) -> reconstructed points lie OFF the plane
//      by a snap-phase-dependent amount -> deterministic distance bands (hypothesis).
//
// Validation: the sim must reproduce ALL FOUR in-game observations before its
// verdict on a new fix is trusted:
//   (a) baseline shows bands (row-mean AO ripple on open floor)
//   (b) horizon-angle bias += 0.25 rad -> ~no change      (in-game no-op)
//   (c) posbias 0.0008 -> ~no change                      (in-game no-op)
//   (d) grazing N.V fade 0.30 -> bands gone               (in-game works)
// Candidate fix under test: SNAP the reconstruction ray to the center of the
// full-res depth texel actually sampled (consistent ray<->depth), fetchPos-only.
//
// Run: node gtao_band_repro.js

const F = Math.fround;

// ---- scene / camera (realistic: 2560x1571 scene, 1280x785 AO like the user's rig)
const FW = 2560, FH = 1571;      // full-res depth
const W = 1280, H = 785;         // half-res AO target (scene/2, floor division)
const zn = 0.2, zf = 350.0;
const m33 = zf / (zf - zn), m43 = -zn * zf / (zf - zn);
const tanY = 0.66, tanX = tanY * (FW / FH);

const add = (a, b) => [a[0]+b[0], a[1]+b[1], a[2]+b[2]];
const sub = (a, b) => [a[0]-b[0], a[1]-b[1], a[2]-b[2]];
const mul = (a, s) => [a[0]*s, a[1]*s, a[2]*s];
const dot = (a, b) => a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
const cross = (a, b) => [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]];
const len = a => Math.hypot(a[0], a[1], a[2]);
const norm = a => mul(a, 1 / len(a));
const clamp = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
const fract = x => x - Math.floor(x);
const smoothstep = (e0, e1, x) => { const t = clamp((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); };

const camPos = [0, 1.6, 0];
const camDir = norm([0, -0.15, 1]);
const camRight = norm(cross([0, 1, 0], camDir));
let camTop = norm(cross(camDir, camRight));
if (camTop[1] < 0) camTop = mul(camTop, -1);

// ---- full-res depth: infinite floor y=0, rasterized at texel centers, stored f32
const depth32 = new Float32Array(FW * FH).fill(1.0);   // f32 = D32 quantization
const depth64 = new Float64Array(FW * FH).fill(1.0);   // control: perfect depth
for (let j = 0; j < FH; ++j) for (let i = 0; i < FW; ++i) {
    const u = (i + 0.5) / FW, v = (j + 0.5) / FH;
    const ndc = [u * 2 - 1, 1 - 2 * v];
    const ray = add(camDir, add(mul(camRight, tanX * ndc[0]), mul(camTop, tanY * ndc[1])));
    if (ray[1] < -1e-9) {
        const t = -camPos[1] / ray[1];               // zview along the unnormalized ray
        if (t > zn && t < zf) {
            const z = m33 + m43 / t;
            depth32[j * FW + i] = F(z);
            depth64[j * FW + i] = z;
        }
    }
}

const SLICES = 4, RADIUS = 4.0;

// opts: { nSample, fp64depth, snapRay, horizonBias, posBias, normBias, grazeFade }
function makeGtao(opts) {
    const dbuf = opts.fp64depth ? depth64 : depth32;
    const invres = [F(1 / W), F(1 / H)];             // pc.res.zw is computed in C++ fp32

    // NEAREST + CLAMP_TO_EDGE texel pick, fp32 uv like the GPU
    const texel = uv => {
        const xi = clamp(Math.floor(F(uv[0]) * FW), 0, FW - 1);
        const yi = clamp(Math.floor(F(uv[1]) * FH), 0, FH - 1);
        return [xi, yi];
    };
    const sampleDepth = uv => { const [xi, yi] = texel(uv); return dbuf[yi * FW + xi]; };

    const fetchPos = uv => {
        const [xi, yi] = texel(uv);
        const zndc = dbuf[yi * FW + xi];
        const zview = clamp(F(F(m43) / F(F(zndc) - F(m33))), 0, 10000);   // fp32 cancellation
        const ruv = opts.snapRay ? [(xi + 0.5) / FW, (yi + 0.5) / FH] : uv;
        const ndc = [ruv[0] * 2 - 1, 1 - 2 * ruv[1]];
        const ray = add(camDir, add(mul(camRight, tanX * ndc[0]), mul(camTop, tanY * ndc[1])));
        return [mul(ray, zview), zview];
    };

    return function gtao(px, py) {
        const uv = [F((px + 0.5) * invres[0]), F((py + 0.5) * invres[1])];
        if (sampleDepth(uv) >= 0.9999) return 1.0;

        const [cPos, cw] = fetchPos(uv);
        const viewV = mul(norm(cPos), -1);

        // NB=2 hybrid centered/one-sided normal (current shader)
        const NB = 2.0;
        const [Rp, Rw] = fetchPos([uv[0] + NB * invres[0], uv[1]]);
        const [Lp, Lw] = fetchPos([uv[0] - NB * invres[0], uv[1]]);
        const [Up, Uw] = fetchPos([uv[0], uv[1] + NB * invres[1]]);
        const [Dp, Dw] = fetchPos([uv[0], uv[1] - NB * invres[1]]);
        const dxR = Math.abs(Rw - cw), dxL = Math.abs(cw - Lw);
        const dyU = Math.abs(Uw - cw), dyD = Math.abs(cw - Dw);
        const ddx = (Math.max(dxR, dxL) > 1.5 * Math.min(dxR, dxL) + 1e-5)
                  ? ((dxR < dxL) ? sub(Rp, cPos) : sub(cPos, Lp))
                  : mul(sub(Rp, Lp), 0.5);
        const ddy = (Math.max(dyU, dyD) > 1.5 * Math.min(dyU, dyD) + 1e-5)
                  ? ((dyU < dyD) ? sub(Up, cPos) : sub(cPos, Dp))
                  : mul(sub(Up, Dp), 0.5);
        let N = cross(ddy, ddx);
        if (len(N) < 1e-12) return 1.0;
        N = norm(N);
        if (dot(N, viewV) < 0) N = mul(N, -1);

        const cPosB = add(mul(cPos, 1 - (opts.posBias || 0)), mul(N, opts.normBias || 0));

        const projScale = H / (2 * tanY);
        const screenRadius = (RADIUS * 0.5 * projScale) / cw;
        // Radial + direction noise. IGN ≈ fract(3.5556·x + 0.309·y) on the pixel
        // lattice — a wrapped 1D gradient with near-repeats every ~9 px in x →
        // a quasi-periodic SCREEN-FIXED pattern ("фильтр") wherever AO is
        // sensitive to the tap radius (grazing ground). Alternatives under test:
        // R2 (the shader's r_ssao_noise 1) and a white-noise integer hash.
        let noiseOffset, noiseDirection;
        if (opts.nonoise) { noiseOffset = 0.5; noiseDirection = 0.5; }
        else if (opts.noiseMode === 'r2') {
            const R2 = [0.75487766624669276, 0.56984029099805327];
            noiseOffset    = fract((px + 5.588238) * R2[0] + (py + 5.588238) * R2[1]);
            noiseDirection = fract(px * R2[0] + py * R2[1]);
        } else if (opts.noiseMode === 'hash') {
            const h = (x, y, s) => {
                let n = (x | 0) * 1664525 + (y | 0) * 1013904223 + s;
                n = (n ^ (n >>> 16)) >>> 0; n = (n * 2246822519) >>> 0;
                n = (n ^ (n >>> 13)) >>> 0; n = (n * 3266489917) >>> 0;
                return ((n ^ (n >>> 16)) >>> 0) / 4294967296;
            };
            noiseOffset    = h(px, py, 747796405);
            noiseDirection = h(px, py, 277803737);
        } else {
            noiseOffset    = fract(52.9829189 * fract((px + 5.588238) * 0.06711056 + (py + 5.588238) * 0.00583715));
            noiseDirection = fract(52.9829189 * fract(px * 0.06711056 + py * 0.00583715));
        }
        const falloffMul = 2 / (RADIUS * RADIUS);
        const nS = opts.nSample;
        const srm = [invres[0] / nS, invres[1] / nS];

        let vis = 0;
        const bent = [0, 0, 0];
        for (let sl = 0; sl < SLICES; ++sl) {
            const phi = (sl + noiseDirection) * Math.PI / SLICES;
            const omega = [Math.cos(phi), Math.sin(phi)];
            const dirV = add(mul(camRight, omega[0]), mul(camTop, omega[1]));
            const axisV = cross(dirV, viewV);
            const projN = sub(N, mul(axisV, dot(N, axisV)));
            const pl = len(projN);
            if (pl < 1e-12) continue;
            const orthoDir = sub(dirV, mul(viewV, dot(dirV, viewV)));
            const sgnN = Math.sign(dot(orthoDir, projN)) || 1;
            const cosN = clamp(dot(projN, viewV) / pl, 0, 1);
            const nang = sgnN * Math.acos(clamp(cosN, -1, 1));
            const sinN2 = 2 * Math.sin(nang);

            const hSide = [0, 0];
            for (let side = 0; side < 2; ++side) {
                const ss = -1 + 2 * side;
                let chc = -1;
                for (let k = 0; k < nS; ++k) {
                    const base = Math.max(screenRadius * (k + noiseOffset), 2 + k);
                    const s = [base * srm[0], base * srm[1]];
                    const st = [F(uv[0] + ss * s[0] * omega[0]), F(uv[1] + ss * s[1] * -omega[1])];
                    const [sp] = fetchPos(st);
                    const hv = sub(sp, cPosB);
                    const d2 = dot(hv, hv);
                    const fall = clamp(d2 * falloffMul, 0, 1);
                    const Hc = d2 > 1e-12 ? dot(mul(hv, 1 / Math.sqrt(d2)), viewV) : -1;
                    if (Hc > chc) chc = Hc * (1 - fall) + chc * fall;
                }
                const hAng = Math.min(Math.acos(clamp(chc, -1, 1)) + (opts.horizonBias || 0), Math.PI);
                const h = nang + clamp(ss * hAng - nang, -Math.PI / 2, Math.PI / 2);
                hSide[side] = h;
                vis += pl * (cosN + h * sinN2 - Math.cos(2 * h - nang)) * 0.25;
            }
            // bent normal (Jimenez slice-bisector, as ssao.frag)
            const bA = (hSide[0] + hSide[1]) * 0.5;
            const tl = len(orthoDir);
            if (tl > 1e-4) {
                const st = mul(orthoDir, 1 / tl);
                bent[0] += (Math.cos(bA) * viewV[0] + Math.sin(bA) * st[0]) * pl;
                bent[1] += (Math.cos(bA) * viewV[1] + Math.sin(bA) * st[1]) * pl;
                bent[2] += (Math.cos(bA) * viewV[2] + Math.sin(bA) * st[2]) * pl;
            }
        }
        let ao = clamp(vis / SLICES, 0, 1);
        if (opts.wantBent) {
            let bn = dot(bent, bent) > 1e-6 ? norm(bent) : N.slice();
            if (opts.bentFade > 0) {   // candidate fix: fade bentN → recon N at grazing (same knob as the AO fade)
                const ndv = clamp(dot(N, viewV), 0, 1);
                const t = smoothstep(opts.bentFade, opts.bentFade + 0.25, ndv);
                bn = norm([N[0] + (bn[0] - N[0]) * t, N[1] + (bn[1] - N[1]) * t, N[2] + (bn[2] - N[2]) * t]);
            }
            return { ao, bn };
        }
        if (opts.grazeFade > 0) {
            const ndv = clamp(dot(N, viewV), 0, 1);
            ao = 1 * (1 - smoothstep(opts.grazeFade, opts.grazeFade + 0.25, ndv)) + ao * smoothstep(opts.grazeFade, opts.grazeFade + 0.25, ndv);
        }
        return ao;
    };
}

// ---- measurement: row-mean AO vs distance (pixel noise averages out; the
// deterministic band signal survives -- same reason the 3x3 blur can't kill it)
const COLS = 160;                       // columns averaged per row
const px0 = Math.floor(W / 2 - COLS / 2);

function profile(gtao) {
    const rows = [];
    for (let py = 0; py < H; ++py) {
        const uv = [(px0 + 0.5) / W, (py + 0.5) / H];
        // zview at row center for labeling
        const ndc = [0, 1 - 2 * (py + 0.5) / H];
        const ray = add(camDir, mul(camTop, tanY * ndc[1]));
        if (ray[1] >= -1e-9) continue;
        const z = -camPos[1] / ray[1];
        if (z < 3 || z > 300) continue;
        let sum = 0;
        for (let c = 0; c < COLS; ++c) sum += gtao(px0 + c, py);
        rows.push({ py, z, mean: sum / COLS });
    }
    return rows;
}

function ripple(rows, z0, z1) {
    const sel = rows.filter(r => r.z >= z0 && r.z < z1).map(r => r.mean);
    if (sel.length < 3) return { n: 0, min: 1, max: 1, amp: 0 };
    return { n: sel.length, min: Math.min(...sel), max: Math.max(...sel),
             amp: Math.max(...sel) - Math.min(...sel) };
}

const variants = [
    ['A baseline (f32 depth, current shader)', { nSample: 2 }],
    ['B perfect fp64 depth (isolate quantization)', { nSample: 2, fp64depth: true }],
    ['C snapRay fix (ray at depth-texel center)', { nSample: 2, snapRay: true }],
    ['D snapRay + fp64 depth', { nSample: 2, snapRay: true, fp64depth: true }],
    ['E horizon bias +0.25 (in-game NO-OP)', { nSample: 2, horizonBias: 0.25 }],
    ['F posbias 0.0008 (in-game NO-OP)', { nSample: 2, posBias: 0.0008 }],
    ['G grazing fade 0.30 (in-game WORKS)', { nSample: 2, grazeFade: 0.30 }],
    ['H baseline @ 4 samples (in-game NO change)', { nSample: 4 }],
];

console.log(`scene ${FW}x${FH} -> AO ${W}x${H}, floor y=0, cam h=1.6 dir(0,${camDir[1].toFixed(3)},${camDir[2].toFixed(3)}), tanY=${tanY}`);
console.log('row-mean AO ripple (max-min) per distance band; open flat floor SHOULD be flat 1.0\n');
console.log('variant'.padEnd(46) + '3-15m'.padStart(8) + '15-40m'.padStart(8) + '40-100m'.padStart(9) + '100-300m'.padStart(9) + '   minAO');
for (const [name, opts] of variants) {
    const rows = profile(makeGtao(opts));
    const r1 = ripple(rows, 3, 15), r2 = ripple(rows, 15, 40), r3 = ripple(rows, 40, 100), r4 = ripple(rows, 100, 300);
    const minAO = Math.min(...rows.map(r => r.mean));
    console.log(name.padEnd(46)
        + r1.amp.toFixed(3).padStart(8) + r2.amp.toFixed(3).padStart(8)
        + r3.amp.toFixed(3).padStart(9) + r4.amp.toFixed(3).padStart(9)
        + minAO.toFixed(3).padStart(9));
}

// eyeball profile for baseline vs snapRay in the band-prone 15-120m range
for (const [name, opts] of [['A baseline', { nSample: 2 }], ['C snapRay', { nSample: 2, snapRay: true }]]) {
    const rows = profile(makeGtao(opts)).filter(r => r.z >= 15 && r.z <= 120);
    console.log(`\n${name} row-mean profile (z: mean), every 4th row:`);
    const out = [];
    for (let i = 0; i < rows.length; i += 4) out.push(`${rows[i].z.toFixed(0)}m:${rows[i].mean.toFixed(3)}`);
    console.log(out.join('  '));
}

// ---- BENT mode: the 2026-07-01 in-game dump showed AO.r is FLAT (~0.95-1.0,
// hp 0.0002) yet the user still sees the screen filter, and it vanishes at
// r_ssao_strength 0 / r_ssao 0. The OTHER payload of that texture is the BENT
// NORMAL (gba) — receivers sample skyAmbient(gtaoBentN()) along it, it gets a
// 3x3 blur but NO temporal EMA, and its per-slice jitter (noiseDirection) can
// imprint the screen-fixed noise into the hemi ambient. Quantify: bent-normal
// angular wobble (after 3x3 renormalized blur) + luminance impact through a
// directional-sky model L(n) = 1 + 0.25 n.y + 0.15 n.x, in display LSBs.
if (process.argv.includes('--bent')) {
    const fs = require('fs');
    const outDir = process.argv[process.argv.indexOf('--bent') + 1] || '.';
    let py0 = 0;
    for (let py = 0; py < H; ++py) {
        const ndc = 1 - 2 * (py + 0.5) / H;
        const ray = add(camDir, mul(camTop, tanY * ndc));
        if (ray[1] < -1e-9 && -camPos[1] / ray[1] < 300) { py0 = py; break; }
    }
    const rows = H - py0;
    const zOfRow = r => {
        const ndc = 1 - 2 * (py0 + r + 0.5) / H;
        const ray = add(camDir, mul(camTop, tanY * ndc));
        return -camPos[1] / ray[1];
    };
    const bentVariants = [
        ['current (no bent fix)', { nSample: 2, wantBent: true }],
        ['bentFade 0.30 (fix B)', { nSample: 2, wantBent: true, bentFade: 0.30 }],
        ['no noise (reference)',  { nSample: 2, wantBent: true, nonoise: true }],
    ];
    console.log('\nbent-normal wobble AFTER 3x3 renormalized blur; sky model L=1+0.25ny+0.15nx; display LSB = |Lhp|*255:');
    console.log('variant'.padEnd(26) + 'deg p50'.padStart(9) + 'deg p95'.padStart(9) + 'LSB p95 8-20m'.padStart(15) + 'LSB p95 20-60m'.padStart(15) + 'LSB max'.padStart(9));
    for (const [name, opts] of bentVariants) {
        const gtao = makeGtao(opts);
        const bx = new Float64Array(W * rows), by = new Float64Array(W * rows), bz = new Float64Array(W * rows);
        for (let r = 0; r < rows; ++r) for (let px = 0; px < W; ++px) {
            const { bn } = gtao(px, py0 + r);
            const i = r * W + px;
            bx[i] = bn[0]; by[i] = bn[1]; bz[i] = bn[2];
        }
        // 3x3 blur + renormalize (pipeline)
        const BX = new Float64Array(W * rows), BY = new Float64Array(W * rows), BZ = new Float64Array(W * rows);
        for (let r = 0; r < rows; ++r) for (let px = 0; px < W; ++px) {
            let sx = 0, sy = 0, sz = 0;
            for (let dy = -1; dy <= 1; ++dy) for (let dx = -1; dx <= 1; ++dx) {
                const yy = Math.max(0, Math.min(rows - 1, r + dy)), xx = Math.max(0, Math.min(W - 1, px + dx));
                sx += bx[yy * W + xx]; sy += by[yy * W + xx]; sz += bz[yy * W + xx];
            }
            const l = Math.hypot(sx, sy, sz) || 1;
            const i = r * W + px;
            BX[i] = sx / l; BY[i] = sy / l; BZ[i] = sz / l;
        }
        // luminance field + its 9x9 high-pass
        const L = new Float64Array(W * rows);
        for (let i = 0; i < W * rows; ++i) L[i] = 1 + 0.25 * BY[i] + 0.15 * BX[i];
        const lp = new Float64Array(W * rows);
        for (let r = 0; r < rows; ++r) for (let px = 0; px < W; ++px) {
            let s = 0, n = 0;
            for (let dy = -4; dy <= 4; ++dy) for (let dx = -4; dx <= 4; ++dx) {
                const yy = r + dy, xx = px + dx;
                if (yy >= 0 && yy < rows && xx >= 0 && xx < W) { s += L[yy * W + xx]; ++n; }
            }
            lp[r * W + px] = s / n;
        }
        // angular wobble vs 9x9 mean direction + per-band LSB stats
        const degs = [], lsb1 = [], lsb2 = []; let lsbMax = 0;
        for (let r = 4; r < rows - 4; ++r) {
            const z = zOfRow(r);
            for (let px = 4; px < W - 4; ++px) {
                const i = r * W + px;
                let mx = 0, my = 0, mz = 0;
                for (let dy = -4; dy <= 4; dy += 2) for (let dx = -4; dx <= 4; dx += 2) {
                    mx += BX[(r + dy) * W + px + dx]; my += BY[(r + dy) * W + px + dx]; mz += BZ[(r + dy) * W + px + dx];
                }
                const ml = Math.hypot(mx, my, mz) || 1;
                const d = Math.acos(Math.min(1, (BX[i] * mx + BY[i] * my + BZ[i] * mz) / ml)) * 180 / Math.PI;
                degs.push(d);
                const lsb = Math.abs(L[i] - lp[i]) * 255;
                lsbMax = Math.max(lsbMax, lsb);
                if (z >= 8 && z < 20) lsb1.push(lsb);
                else if (z >= 20 && z < 60) lsb2.push(lsb);
            }
        }
        degs.sort((a, b) => a - b); lsb1.sort((a, b) => a - b); lsb2.sort((a, b) => a - b);
        const q = (a, p) => a.length ? a[Math.floor(a.length * p)] : 0;
        console.log(name.padEnd(26)
            + q(degs, 0.5).toFixed(2).padStart(9) + q(degs, 0.95).toFixed(2).padStart(9)
            + q(lsb1, 0.95).toFixed(1).padStart(15) + q(lsb2, 0.95).toFixed(1).padStart(15)
            + lsbMax.toFixed(1).padStart(9));
        // luminance high-pass image, mid-gray ±3 LSB
        const buf = Buffer.alloc(W * rows);
        for (let i = 0; i < W * rows; ++i)
            buf[i] = Math.max(0, Math.min(255, Math.round(128 + (L[i] - lp[i]) * 255 * 42)));
        const tag = name.replace(/[^a-z0-9]+/gi, '_');
        fs.writeFileSync(`${outDir}/bent_${tag}.pgm`, Buffer.concat([Buffer.from(`P5\n${W} ${rows}\n255\n`), buf]));
    }
    process.exit(0);
}

// ---- FILTER mode: quantify the SCREEN-FIXED pattern ("фильтр на экране") the
// user sees on the ground. Render the floor AO, 3x3-blur it (the real pipeline's
// spatial filter), then HIGH-PASS (blurred - 9x9 mean): the residual is the
// structured noise imprint. Report its p95 amplitude in DISPLAY LSBs (through
// the receivers' pow(ao,2) at strength 2, x255) per distance band — >2-3 LSB of
// CORRELATED structure = a visible filter no output dither can hide.
if (process.argv.includes('--filter')) {
    const fs = require('fs');
    const outDir = process.argv[process.argv.indexOf('--filter') + 1] || '.';
    let py0 = 0;
    for (let py = 0; py < H; ++py) {
        const ndc = 1 - 2 * (py + 0.5) / H;
        const ray = add(camDir, mul(camTop, tanY * ndc));
        if (ray[1] < -1e-9 && -camPos[1] / ray[1] < 300) { py0 = py; break; }
    }
    const rows = H - py0;
    const zOfRow = r => {
        const ndc = 1 - 2 * (py0 + r + 0.5) / H;
        const ray = add(camDir, mul(camTop, tanY * ndc));
        return -camPos[1] / ray[1];
    };
    const filterVariants = [
        ['IGN (current)',        { nSample: 2 }],
        ['R2 (r_ssao_noise 1)',  { nSample: 2, noiseMode: 'r2' }],
        ['white hash',           { nSample: 2, noiseMode: 'hash' }],
        ['IGN + snapRay',        { nSample: 2, snapRay: true }],
        ['hash + snapRay',       { nSample: 2, noiseMode: 'hash', snapRay: true }],
        ['no noise (floor ref)', { nSample: 2, nonoise: true }],
    ];
    console.log('\nhigh-pass pattern amplitude AFTER 3x3 blur, in display LSBs (pow(ao,2)*255), p95|max:');
    console.log('variant'.padEnd(24) + '3-8m'.padStart(12) + '8-20m'.padStart(12) + '20-60m'.padStart(12) + '60-300m'.padStart(12));
    for (const [name, opts] of filterVariants) {
        const gtao = makeGtao(opts);
        const raw = new Float64Array(W * rows);
        for (let r = 0; r < rows; ++r)
            for (let px = 0; px < W; ++px) raw[r * W + px] = gtao(px, py0 + r);
        // 3x3 blur (pipeline)
        const bl = new Float64Array(W * rows);
        for (let r = 0; r < rows; ++r) for (let px = 0; px < W; ++px) {
            let s = 0, n = 0;
            for (let dy = -1; dy <= 1; ++dy) for (let dx = -1; dx <= 1; ++dx) {
                const yy = r + dy, xx = px + dx;
                if (yy >= 0 && yy < rows && xx >= 0 && xx < W) { s += raw[yy * W + xx]; ++n; }
            }
            bl[r * W + px] = s / n;
        }
        // 9x9 mean = local low-pass
        const lp = new Float64Array(W * rows);
        for (let r = 0; r < rows; ++r) for (let px = 0; px < W; ++px) {
            let s = 0, n = 0;
            for (let dy = -4; dy <= 4; ++dy) for (let dx = -4; dx <= 4; ++dx) {
                const yy = r + dy, xx = px + dx;
                if (yy >= 0 && yy < rows && xx >= 0 && xx < W) { s += bl[yy * W + xx]; ++n; }
            }
            lp[r * W + px] = s / n;
        }
        const bands = [[3, 8], [8, 20], [20, 60], [60, 300]];
        const cells = bands.map(([z0, z1]) => {
            const amps = [];
            for (let r = 4; r < rows - 4; ++r) {
                const z = zOfRow(r);
                if (z < z0 || z >= z1) continue;
                for (let px = 4; px < W - 4; ++px) {
                    const i = r * W + px;
                    amps.push(Math.abs(Math.pow(bl[i], 2) - Math.pow(lp[i], 2)) * 255);
                }
            }
            if (!amps.length) return '      -';
            amps.sort((a, b) => a - b);
            const p95 = amps[Math.floor(amps.length * 0.95)], mx = amps[amps.length - 1];
            return `${p95.toFixed(1)}|${mx.toFixed(1)}`;
        });
        console.log(name.padEnd(24) + cells.map(c => c.padStart(12)).join(''));
        // x4 zoomed crop of the high-pass at 8-20m, hard-stretched: mid-gray ± 3 LSB
        let r0 = 4; while (r0 < rows - 4 && zOfRow(r0) > 20) ++r0;
        const CW = 240, CH = 90, SC = 4;
        const cx0 = Math.floor(W / 2 - CW / 2);
        const buf = Buffer.alloc(CW * SC * CH * SC);
        for (let r = 0; r < CH; ++r) for (let px = 0; px < CW; ++px) {
            const i = (r0 + r) * W + cx0 + px;
            const hpLSB = (Math.pow(bl[i], 2) - Math.pow(lp[i], 2)) * 255;
            const v = Math.max(0, Math.min(255, Math.round(128 + hpLSB * 42)));   // ±3 LSB full range
            for (let sy = 0; sy < SC; ++sy) for (let sx = 0; sx < SC; ++sx)
                buf[(r * SC + sy) * CW * SC + px * SC + sx] = v;
        }
        const tag = name.replace(/[^a-z0-9]+/gi, '_');
        fs.writeFileSync(`${outDir}/hp_${tag}.pgm`,
            Buffer.concat([Buffer.from(`P5\n${CW * SC} ${CH * SC}\n255\n`), buf]));
    }
    process.exit(0);
}

// ---- IMAGE mode: render the AO of the floor region to PGM, displayed like the
// user sees it (pow(ao, strength) then 8-bit quantize), plus a contrast-stretched
// version (0.9..1.0 -> black..white) to expose sub-1% structure. Bands that are
// invisible in row-means (column-phase-varying) show up here.
if (process.argv.includes('--image')) {
    const fs = require('fs');
    const outDir = process.argv[process.argv.indexOf('--image') + 1] || '.';
    // floor rows only
    let py0 = 0;
    for (let py = 0; py < H; ++py) {
        const ndc = 1 - 2 * (py + 0.5) / H;
        const ray = add(camDir, mul(camTop, tanY * ndc));
        if (ray[1] < -1e-9 && -camPos[1] / ray[1] < 300) { py0 = py; break; }
    }
    const rows = H - py0;
    const imgVariants = [
        ['A_baseline', { nSample: 2 }],
        ['C_snapRay',  { nSample: 2, snapRay: true }],
        ['N_nonoise',  { nSample: 2, nonoise: true }],
    ];
    for (const [name, opts] of imgVariants) {
        const gtao = makeGtao(opts);
        const raw = new Float64Array(W * rows);
        for (let r = 0; r < rows; ++r) {
            for (let px = 0; px < W; ++px) raw[r * W + px] = gtao(px, py0 + r);
        }
        // simulated 3x3 box blur (stand-in for the depth-aware blur on a plane
        // where all depth weights ~1)
        const blurred = new Float64Array(W * rows);
        for (let r = 0; r < rows; ++r) for (let px = 0; px < W; ++px) {
            let s = 0, n = 0;
            for (let dy = -1; dy <= 1; ++dy) for (let dx = -1; dx <= 1; ++dx) {
                const yy = r + dy, xx = px + dx;
                if (yy >= 0 && yy < rows && xx >= 0 && xx < W) { s += raw[yy * W + xx]; ++n; }
            }
            blurred[r * W + px] = s / n;
        }
        const writePGM = (file, fn) => {
            const buf = Buffer.alloc(W * rows);
            for (let i = 0; i < W * rows; ++i) buf[i] = Math.max(0, Math.min(255, Math.round(fn(blurred[i]))));
            fs.writeFileSync(file, Buffer.concat([Buffer.from(`P5\n${W} ${rows}\n255\n`), buf]));
        };
        // as-displayed: ambient multiplier pow(ao,2) mapped to full 8-bit
        writePGM(`${outDir}/${name}_display.pgm`, ao => Math.pow(ao, 2) * 255);
        // stretched: 0.90..1.00 -> 0..255
        writePGM(`${outDir}/${name}_stretch.pgm`, ao => (ao - 0.90) * 2550);
        let mn = 1, mx = 0;
        for (let i = 0; i < W * rows; ++i) { mn = Math.min(mn, blurred[i]); mx = Math.max(mx, blurred[i]); }
        console.log(`${name}: blurred AO range [${mn.toFixed(4)}, ${mx.toFixed(4)}] over ${W}x${rows}`);
    }
}
