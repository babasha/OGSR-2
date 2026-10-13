// TGA -> PNG + mean/diff stats. Node, no deps.
// usage: node tga.js png <in.tga> <out.png> [scaleDiv]
//        node tga.js stats <a.tga> [b.tga]
const fs = require('fs');
const zlib = require('zlib');

function readTGA(path) {
  const b = fs.readFileSync(path);
  const idLen = b[0], cmType = b[1], imgType = b[2];
  const w = b.readUInt16LE(12), h = b.readUInt16LE(14);
  const bpp = b[16], desc = b[17];
  if (cmType !== 0) throw new Error('colour-mapped TGA not supported');
  let off = 18 + idLen;
  const px = bpp / 8;
  const out = Buffer.alloc(w * h * 3);
  const topDown = (desc & 0x20) !== 0;
  const put = (x, y, r, g, bl) => {
    const yy = topDown ? y : (h - 1 - y);
    const i = (yy * w + x) * 3;
    out[i] = r; out[i + 1] = g; out[i + 2] = bl;
  };
  if (imgType === 2) {                    // uncompressed truecolour
    for (let y = 0; y < h; ++y)
      for (let x = 0; x < w; ++x) {
        const i = off + (y * w + x) * px;
        put(x, y, b[i + 2], b[i + 1], b[i]);
      }
  } else if (imgType === 10) {            // RLE truecolour
    let i = off, n = 0;
    while (n < w * h) {
      const hdr = b[i++];
      const cnt = (hdr & 0x7f) + 1;
      if (hdr & 0x80) {
        const r = b[i + 2], g = b[i + 1], bl = b[i]; i += px;
        for (let k = 0; k < cnt; ++k, ++n) put(n % w, (n / w) | 0, r, g, bl);
      } else {
        for (let k = 0; k < cnt; ++k, ++n, i += px) put(n % w, (n / w) | 0, b[i + 2], b[i + 1], b[i]);
      }
    }
  } else throw new Error('TGA image type ' + imgType + ' not supported');
  return { w, h, rgb: out };
}

function crc32(buf) {
  let c, table = crc32.t || (crc32.t = (() => {
    const t = new Int32Array(256);
    for (let n = 0; n < 256; n++) { c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c; }
    return t;
  })());
  let crc = -1;
  for (let i = 0; i < buf.length; i++) crc = (crc >>> 8) ^ table[(crc ^ buf[i]) & 0xFF];
  return (crc ^ -1) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}

function writePNG(path, w, h, rgb) {
  const raw = Buffer.alloc(h * (w * 3 + 1));
  for (let y = 0; y < h; ++y) {
    raw[y * (w * 3 + 1)] = 0;
    rgb.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  fs.writeFileSync(path, Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 6 })), chunk('IEND', Buffer.alloc(0))]));
}

function downscale(img, d) {
  if (d <= 1) return img;
  const w = Math.floor(img.w / d), h = Math.floor(img.h / d);
  const out = Buffer.alloc(w * h * 3);
  for (let y = 0; y < h; ++y)
    for (let x = 0; x < w; ++x) {
      let r = 0, g = 0, b = 0;
      for (let j = 0; j < d; ++j) for (let i = 0; i < d; ++i) {
        const k = ((y * d + j) * img.w + (x * d + i)) * 3;
        r += img.rgb[k]; g += img.rgb[k + 1]; b += img.rgb[k + 2];
      }
      const n = d * d, k = (y * w + x) * 3;
      out[k] = r / n; out[k + 1] = g / n; out[k + 2] = b / n;
    }
  return { w, h, rgb: out };
}

function mean(img) {
  let r = 0, g = 0, b = 0;
  for (let i = 0; i < img.rgb.length; i += 3) { r += img.rgb[i]; g += img.rgb[i + 1]; b += img.rgb[i + 2]; }
  const n = img.rgb.length / 3;
  return [r / n, g / n, b / n];
}

const [, , mode, a, bArg, scale] = process.argv;
if (mode === 'png') {
  const img = downscale(readTGA(a), parseInt(scale || '2', 10));
  writePNG(bArg, img.w, img.h, img.rgb);
  const m = mean(img);
  console.log(`${bArg} ${img.w}x${img.h} mean rgb ${m.map(v => v.toFixed(2)).join(' ')}`);
} else if (mode === 'stats') {
  const A = readTGA(a);
  const mA = mean(A);
  console.log(`A ${a} ${A.w}x${A.h} mean ${mA.map(v => v.toFixed(3)).join(' ')} luma ${(0.2126*mA[0]+0.7152*mA[1]+0.0722*mA[2]).toFixed(3)}`);
  if (bArg) {
    const B = readTGA(bArg);
    const mB = mean(B);
    console.log(`B ${bArg} ${B.w}x${B.h} mean ${mB.map(v => v.toFixed(3)).join(' ')} luma ${(0.2126*mB[0]+0.7152*mB[1]+0.0722*mB[2]).toFixed(3)}`);
    let sad = 0, maxd = 0, over = 0;
    for (let i = 0; i < A.rgb.length; ++i) {
      const d = Math.abs(A.rgb[i] - B.rgb[i]);
      sad += d; if (d > maxd) maxd = d; if (d > 8) ++over;
    }
    console.log(`diff: mean |A-B| ${(sad / A.rgb.length).toFixed(3)} of 255, max ${maxd}, ${(100 * over / A.rgb.length).toFixed(2)}% of channels differ by >8`);
    console.log(`delta luma B-A: ${((0.2126*(mB[0]-mA[0])+0.7152*(mB[1]-mA[1])+0.0722*(mB[2]-mA[2]))).toFixed(3)} of 255`);
  }
} else {
  console.log('usage: node tga.js png <in.tga> <out.png> [div] | node tga.js stats <a.tga> [b.tga]');
}

// appended: signed difference view. node tga.js diff a.tga b.tga out.png [gain] [div]
if (mode === 'diff') {
  const A = readTGA(a), B = readTGA(bArg);
  const outPath = process.argv[5], gain = parseFloat(process.argv[6] || '8'), d = parseInt(process.argv[7] || '2', 10);
  const rgb = Buffer.alloc(A.rgb.length);
  for (let i = 0; i < A.rgb.length; i += 3) {
    // luma delta, red = B brighter, blue = B darker, grey = equal
    const la = 0.2126*A.rgb[i] + 0.7152*A.rgb[i+1] + 0.0722*A.rgb[i+2];
    const lb = 0.2126*B.rgb[i] + 0.7152*B.rgb[i+1] + 0.0722*B.rgb[i+2];
    const dv = (lb - la) * gain;
    rgb[i]   = Math.max(0, Math.min(255, 128 + dv));
    rgb[i+1] = Math.max(0, Math.min(255, 128 - Math.abs(dv) * 0.5));
    rgb[i+2] = Math.max(0, Math.min(255, 128 - dv));
  }
  const img = downscale({ w: A.w, h: A.h, rgb }, d);
  writePNG(outPath, img.w, img.h, img.rgb);
  console.log(`${outPath} gain x${gain} (red = gather brighter, blue = darker)`);
}
