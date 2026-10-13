// Viewer/analyzer for the in-game AO dump (vk_pass_ssao.cpp writes
// <game>\bin_x64\ssao_dump.bin while r_ssao_debug is active: u32 w, u32 h,
// then w*h R16F halfs = the FINAL blurred AO the receivers sample).
// The floor "screen filter" hunt: the pattern's amplitude, spatial period and
// orientation discriminate the mechanism (IGN imprint ~9px quasi-period /
// 2px half-res upsample grid / smooth gradient = display-side).
//
// Usage: node ssao_dump_view.js <ssao_dump.bin> [outDir]
// Emits: <out>/ao_full.png    — AO stretched 0.6..1.0
//        <out>/ao_hp.png      — high-pass (AO - 9x9 mean), mid-gray ±0.02
//        <out>/ao_hp_zoom.png — x4 zoom of the high-pass, bottom-center crop
//        console: histogram + high-pass amplitude + x/y autocorrelation peaks
const fs = require('fs'), zlib = require('zlib');

const file = process.argv[2];
if (!file) { console.error('usage: node ssao_dump_view.js <ssao_dump.bin> [outDir]'); process.exit(1); }
const outDir = process.argv[3] || '.';
const raw = fs.readFileSync(file);
const w = raw.readUInt32LE(0), h = raw.readUInt32LE(4);
const chans = Math.round((raw.length - 8) / (w * h * 2));   // 1 = R only (old), 4 = RGBA (AO + bentN)
console.log(`dump: ${w}x${h}, ${chans} channel(s)`);
const half2f = u => {
    const s = (u & 0x8000) ? -1 : 1, e = (u >> 10) & 0x1F, m = u & 0x3FF;
    if (e === 0) return s * m * Math.pow(2, -24);
    if (e === 31) return m ? NaN : s * Infinity;
    return s * (1 + m / 1024) * Math.pow(2, e - 15);
};
const ao = new Float64Array(w * h);
for (let i = 0; i < w * h; ++i) ao[i] = half2f(raw.readUInt16LE(8 + i * chans * 2));
let BX = null, BY = null, BZ = null;
if (chans === 4) {
    BX = new Float64Array(w * h); BY = new Float64Array(w * h); BZ = new Float64Array(w * h);
    for (let i = 0; i < w * h; ++i) {
        BX[i] = half2f(raw.readUInt16LE(8 + (i * 4 + 1) * 2)) * 2 - 1;
        BY[i] = half2f(raw.readUInt16LE(8 + (i * 4 + 2) * 2)) * 2 - 1;
        BZ[i] = half2f(raw.readUInt16LE(8 + (i * 4 + 3) * 2)) * 2 - 1;
    }
}

// histogram
{
    const bins = new Array(21).fill(0);
    for (let i = 0; i < w * h; ++i) bins[Math.max(0, Math.min(20, Math.floor(ao[i] * 20)))]++;
    console.log('histogram (0..1 in 20 bins, % of pixels):');
    console.log(bins.map((b, i) => `${(i * 0.05).toFixed(2)}:${(100 * b / (w * h)).toFixed(1)}`).join(' '));
}

// 9x9 mean low-pass -> high-pass
const lp = new Float64Array(w * h);
{
    // separable box 9
    const tmp = new Float64Array(w * h);
    for (let y = 0; y < h; ++y) for (let x = 0; x < w; ++x) {
        let s = 0, n = 0;
        for (let d = -4; d <= 4; ++d) { const xx = x + d; if (xx >= 0 && xx < w) { s += ao[y * w + xx]; ++n; } }
        tmp[y * w + x] = s / n;
    }
    for (let y = 0; y < h; ++y) for (let x = 0; x < w; ++x) {
        let s = 0, n = 0;
        for (let d = -4; d <= 4; ++d) { const yy = y + d; if (yy >= 0 && yy < h) { s += tmp[yy * w + x]; ++n; } }
        lp[y * w + x] = s / n;
    }
}
const hp = new Float64Array(w * h);
for (let i = 0; i < w * h; ++i) hp[i] = ao[i] - lp[i];

// high-pass amplitude in thirds of the screen height (top/mid/bottom)
for (const [name, y0, y1] of [['top', 0, h / 3], ['mid', h / 3, 2 * h / 3], ['bottom', 2 * h / 3, h]]) {
    const a = [];
    for (let y = Math.ceil(y0) + 4; y < y1 - 4; ++y)
        for (let x = 4; x < w - 4; ++x) a.push(Math.abs(hp[y * w + x]));
    a.sort((p, q) => p - q);
    console.log(`${name}: hp p50 ${a[a.length >> 1].toFixed(4)}  p95 ${a[Math.floor(a.length * 0.95)].toFixed(4)}  max ${a[a.length - 1].toFixed(4)}  (AO units; display ~= x2 x255 LSB)`);
}

// autocorrelation of the high-pass along x and along y (bottom half = floor),
// lag 1..24 — a periodic "filter" shows as a positive bump at its period
{
    const y0 = Math.floor(h / 2), corr = (dx, dy) => {
        let s = 0, s2 = 0, n = 0;
        for (let y = y0; y < h - 24; ++y) for (let x = 0; x < w - 24; x += 2) {
            const a = hp[y * w + x], b = hp[(y + dy) * w + x + dx];
            s += a * b; s2 += a * a; ++n;
        }
        return s / Math.max(s2, 1e-12);
    };
    const ax = [], ay = [], ad = [];
    for (let l = 1; l <= 24; ++l) { ax.push(corr(l, 0).toFixed(2)); ay.push(corr(0, l).toFixed(2)); ad.push(corr(l, l).toFixed(2)); }
    console.log('autocorr x  1..24:', ax.join(' '));
    console.log('autocorr y  1..24:', ay.join(' '));
    console.log('autocorr xy 1..24:', ad.join(' '));
}

// PNG writer (grayscale)
function crc32(buf) {
    let c, t = [];
    for (let n = 0; n < 256; n++) { c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c; }
    let crc = 0xFFFFFFFF;
    for (let i = 0; i < buf.length; i++) crc = t[(crc ^ buf[i]) & 0xFF] ^ (crc >>> 8);
    return (crc ^ 0xFFFFFFFF) >>> 0;
}
function chunk(type, data) {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const cc = Buffer.alloc(4); cc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, cc]);
}
function writePNG(path, ww, hh, fn) {
    const rows = [];
    for (let y = 0; y < hh; ++y) {
        const r = Buffer.alloc(ww + 1);
        for (let x = 0; x < ww; ++x) r[x + 1] = Math.max(0, Math.min(255, Math.round(fn(x, y))));
        rows.push(r);
    }
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(ww, 0); ihdr.writeUInt32BE(hh, 4); ihdr[8] = 8;
    fs.writeFileSync(path, Buffer.concat([
        Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
        chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(Buffer.concat(rows))), chunk('IEND', Buffer.alloc(0))]));
    console.log('wrote', path);
}
writePNG(`${outDir}/ao_full.png`, w, h, (x, y) => (ao[y * w + x] - 0.6) * 637.5);
writePNG(`${outDir}/ao_hp.png`, w, h, (x, y) => 128 + hp[y * w + x] * 6375);
const CW = 240, CH = 120, SC = 4, cx0 = (w - CW) >> 1, cy0 = h - CH - 10;
writePNG(`${outDir}/ao_hp_zoom.png`, CW * SC, CH * SC,
    (x, y) => 128 + hp[(cy0 + (y / SC | 0)) * w + cx0 + (x / SC | 0)] * 6375);

// ---- bent normal (4-channel dumps): wobble + luminance-model pattern --------
if (chans === 4) {
    // luminance model: directional sky L(n) = 1 + 0.25 n.y + 0.15 n.x — a proxy
    // for skyAmbient(bentN); its high-pass is what the eye would see on ground.
    const L = new Float64Array(w * h);
    for (let i = 0; i < w * h; ++i) {
        const l = Math.hypot(BX[i], BY[i], BZ[i]);
        L[i] = l > 0.25 ? 1 + 0.25 * BY[i] / l + 0.15 * BX[i] / l : 1;
    }
    const lpL = new Float64Array(w * h);
    {
        const tmp = new Float64Array(w * h);
        for (let y = 0; y < h; ++y) for (let x = 0; x < w; ++x) {
            let s = 0, n = 0;
            for (let d = -4; d <= 4; ++d) { const xx = x + d; if (xx >= 0 && xx < w) { s += L[y * w + xx]; ++n; } }
            tmp[y * w + x] = s / n;
        }
        for (let y = 0; y < h; ++y) for (let x = 0; x < w; ++x) {
            let s = 0, n = 0;
            for (let d = -4; d <= 4; ++d) { const yy = y + d; if (yy >= 0 && yy < h) { s += tmp[yy * w + x]; ++n; } }
            lpL[y * w + x] = s / n;
        }
    }
    for (const [name, y0, y1] of [['top', 0, h / 3], ['mid', h / 3, 2 * h / 3], ['bottom', 2 * h / 3, h]]) {
        const a = [];
        for (let y = Math.ceil(y0) + 4; y < y1 - 4; ++y)
            for (let x = 4; x < w - 4; ++x) a.push(Math.abs(L[y * w + x] - lpL[y * w + x]) * 255);
        a.sort((p, q) => p - q);
        console.log(`bentN sky-model ${name}: hp p50 ${a[a.length >> 1].toFixed(2)}  p95 ${a[Math.floor(a.length * 0.95)].toFixed(2)}  max ${a[a.length - 1].toFixed(2)} display LSB`);
    }
    writePNG(`${outDir}/bent_ny.png`, w, h, (x, y) => (BY[y * w + x] * 0.5 + 0.5) * 255);
    writePNG(`${outDir}/bent_hp.png`, w, h, (x, y) => 128 + (L[y * w + x] - lpL[y * w + x]) * 255 * 42);
    writePNG(`${outDir}/bent_hp_zoom.png`, CW * SC, CH * SC,
        (x, y) => 128 + (L[(cy0 + (y / SC | 0)) * w + cx0 + (x / SC | 0)] - lpL[(cy0 + (y / SC | 0)) * w + cx0 + (x / SC | 0)]) * 255 * 42);
}
