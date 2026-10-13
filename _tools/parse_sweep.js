// Bucket a sweep log by [VK AutoShot] cmd markers and report each step's steady
// state. Reads the MEDIAN of the bucket's [VK Frame] windows, not the last one:
// one 5 s window is not a measurement in a scene that animates, and the spread
// column is there so a step whose windows disagree cannot look like a result.
// The first window of a bucket is dropped - it straddles the command.
// usage: node parse_sweep.js <log> [zone,zone,...]
const fs = require('fs');
const log = process.argv[2];
const zones = (process.argv[3] || 'DDGI,World,WRC/Shade,Gather/Trace,DDGI/Trace,Trees,World/VSMmark,World/VSMrender,World/SSAO,World/Depth,World/Color,Grass')
  .split(',').map(s => s.trim()).filter(Boolean);

const lines = fs.readFileSync(log, 'latin1').split(/\r?\n/);
const buckets = [{ name: '(warmup)', frame: [], perf: [], cpuphase: [] }];
for (const L of lines) {
  const m = L.match(/\[VK AutoShot\] cmd: (.+)$/);
  if (m) { buckets.push({ name: m[1].trim(), frame: [], perf: [], cpuphase: [] }); continue; }
  const b = buckets[buckets.length - 1];
  if (L.includes('[VK Frame]')) b.frame.push(L);
  else if (L.includes('[VK Perf]') && L.includes('gpu_total')) b.perf.push(L);
  else if (L.includes('[VK CPUphase]')) b.cpuphase.push(L);
}

const num = (s, re) => { const m = s.match(re); return m ? parseFloat(m[1]) : NaN; };
const pad = (s, n) => String(s).padEnd(n);
const median = a => { const v = [...a].sort((x, y) => x - y); const h = v.length >> 1;
  return v.length % 2 ? v[h] : (v[h - 1] + v[h]) / 2; };

// zone=cur(min/max) -> the MAX over the window, by plain string search. `cur` is
// one frame and swings by 3x on the tree pass alone; min/max bracket the window.
function zoneStat(perf, name) {
  const key = ' ' + name + '=';
  const i = perf.indexOf(key);
  if (i < 0) return null;
  const open = perf.indexOf('(', i + key.length);
  const close = perf.indexOf(')', open);
  if (open < 0 || close < 0) return null;
  const cur = perf.slice(i + key.length, open);
  const mm = perf.slice(open + 1, close).split('/');
  return /^[\d.]+$/.test(cur) ? { cur: +cur, min: +mm[0], max: +mm[1] } : null;
}

console.log('step'.padEnd(26) + 'win  fps_med  fps(lo-hi)   cpu_med  gpu_med  jit');
console.log('-'.repeat(80));
const rows = [];
for (const b of buckets) {
  // Drop the window the command landed inside: it is half one setting, half the other.
  const w = b.frame.length > 1 ? b.frame.slice(1) : b.frame;
  if (!w.length) { console.log(pad(b.name, 26) + '  (no window)'); rows.push(null); continue; }
  const fa = w.map(f => num(f, /fps_avg=(\d+)/));
  const ca = w.map(f => num(f, /cpu avg=([\d.]+)/));
  const ga = w.map(f => num(f, /gpu avg=([\d.]+)/));
  const jj = w.map(f => num(f, /jitter=([\d.]+)/));
  const r = { name: b.name, n: w.length, fa: median(fa), lo: Math.min(...fa), hi: Math.max(...fa),
              ca: median(ca), ga: median(ga), j: median(jj),
              perf: b.perf.length ? b.perf[b.perf.length - 1] : '' };
  rows.push(r);
  console.log(pad(r.name, 26) + pad(r.n, 5) + pad(r.fa, 9) + pad(`${r.lo}-${r.hi}`, 13) +
              pad(r.ca.toFixed(1), 9) + pad(r.ga.toFixed(1), 9) + r.j);
}
console.log('\nGPU zones, ms - "max over the last window" (cur is ONE frame and lies):');
const hdr = 'step'.padEnd(26) + zones.map(z => pad(z.length > 9 ? z.slice(-9) : z, 10)).join('');
console.log(hdr);
console.log('-'.repeat(hdr.length));
for (const r of rows) {
  if (!r || !r.perf) continue;
  let out = pad(r.name, 26);
  for (const z of zones) { const s = zoneStat(r.perf, z); out += pad(s ? s.max.toFixed(2) : '-', 10); }
  console.log(out);
}
