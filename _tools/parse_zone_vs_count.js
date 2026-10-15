// Per-bucket zone value NEXT TO the crown count that produced it. A tree zone in
// ms is only comparable between buckets if the same number of crowns was drawn:
// "the pass got faster" and "the camera drifted off the forest" are the same
// number otherwise, and this project has been burned by exactly that.
const fs = require('fs');
const log = process.argv[2], zone = process.argv[3] || 'Trees/Mesh';
const key = ' ' + zone + '=';
const lines = fs.readFileSync(log, 'latin1').split(/\r?\n/);
let b = [{ name: '(warmup)', ms: [], n: [] }];
for (const L of lines) {
  const c = L.match(/\[VK AutoShot\] cmd: (.+)$/);
  if (c) { b.push({ name: c[1].trim(), ms: [], n: [] }); continue; }
  const d = L.match(/\[VK Trees\] forward drawn: (\d+) of/);
  if (d) { b[b.length - 1].n.push(+d[1]); continue; }
  if (!L.includes('[VK Perf]') || !L.includes('gpu_total')) continue;
  const i = L.indexOf(key); if (i < 0) continue;
  const v = parseFloat(L.slice(i + key.length, L.indexOf('(', i + key.length)));
  if (!isNaN(v)) b[b.length - 1].ms.push(v);
}
const med = a => { const s = [...a].sort((x, y) => x - y), h = s.length >> 1;
  return s.length ? (s.length % 2 ? s[h] : (s[h - 1] + s[h]) / 2) : NaN; };
console.log('step                    ' + zone.padEnd(10) + ' crowns      ms/1k crowns');
for (const x of b) {
  const m = med(x.ms), n = med(x.n);
  console.log(x.name.padEnd(23),
    (isNaN(m) ? '  -  ' : m.toFixed(2)).padStart(6),
    '   ', (isNaN(n) ? '  -  ' : String(n)).padStart(6),
    '   ', (isNaN(m) || isNaN(n) ? '  -  ' : (1000 * m / n).toFixed(3)).padStart(7));
}
