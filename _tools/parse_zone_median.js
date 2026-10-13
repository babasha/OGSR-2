// Per-bucket MEDIAN of the `cur` sample of one zone, plus n and spread.
// parse_sweep prints "max over the last window", and the tree zone's max is the
// frame the VSM atlas refreshed in - not the steady state this ladder is about.
const fs = require('fs');
const log = process.argv[2], zone = process.argv[3] || 'Trees/Mesh';
const key = ' ' + zone + '=';
const lines = fs.readFileSync(log, 'latin1').split(/\r?\n/);
let buckets = [{ name: '(warmup)', v: [] }];
for (const L of lines) {
  const m = L.match(/\[VK AutoShot\] cmd: (.+)$/);
  if (m) { buckets.push({ name: m[1].trim(), v: [] }); continue; }
  if (!L.includes('[VK Perf]') || !L.includes('gpu_total')) continue;
  const i = L.indexOf(key); if (i < 0) continue;
  const open = L.indexOf('(', i + key.length);
  const cur = parseFloat(L.slice(i + key.length, open));
  if (!isNaN(cur)) buckets[buckets.length - 1].v.push(cur);
}
const med = a => { const s = [...a].sort((x, y) => x - y), h = s.length >> 1;
  return s.length % 2 ? s[h] : (s[h - 1] + s[h]) / 2; };
console.log(zone + ' (cur, per bucket)');
console.log('step                    n   med    min    max');
for (const b of buckets) {
  if (!b.v.length) { console.log(b.name.padEnd(23), '0   (no samples)'); continue; }
  console.log(b.name.padEnd(23),
    String(b.v.length).padEnd(3),
    med(b.v).toFixed(2).padStart(5),
    Math.min(...b.v).toFixed(2).padStart(6),
    Math.max(...b.v).toFixed(2).padStart(6));
}
