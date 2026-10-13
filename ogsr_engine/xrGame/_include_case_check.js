// Checks that every #include "..." in xrGame/COMMON_AI resolves with EXACT case, the way it would on
// a case-sensitive Linux filesystem. The WSL harness builds from NTFS/DrvFs, which is case-insensitive,
// so it cannot see these. Resolution order mirrors GCC for quoted includes: includer's directory, then -I list.
// Usage: node _include_case_check.js            (from anywhere)
const fs = require('fs'), path = require('path');
const ROOT = path.resolve(__dirname, '..');
const TP = path.resolve(ROOT, '../3rd_party/Src');
const IDIRS = ['xrGame', 'COMMON_AI', 'COMMON_AI/PATH', 'COMMON_AI/GRAPH', 'COMMON_AI/ALGORITHMS', 'xrCore', 'xr_3da', 'xrCDB', 'xrSound',
  'xrParticles', 'Luabind', 'LuaJIT/src', 'xrOnlineNet', 'Include'].map(d => path.join(ROOT, d))
  .concat(['ode', 'tracy/public', 'imgui', 'RmlUi/Include', 'zstd/zstd/lib'].map(d => path.join(TP, d)));
const dirCache = new Map();
function list(d) { if (!dirCache.has(d)) { try { dirCache.set(d, new Set(fs.readdirSync(d))); } catch { dirCache.set(d, null); } } return dirCache.get(d); }
// exact-case existence check, component by component (".." and "." are resolved lexically first)
function existsExact(base, rel) {
  const full = path.resolve(base, rel);
  const parts = path.relative(path.parse(full).root, full).split(path.sep);
  let cur = path.parse(full).root;
  for (const p of parts) { const l = list(cur); if (!l || !l.has(p)) return false; cur = path.join(cur, p); }
  return true;
}
function existsAnyCase(base, rel) { return fs.existsSync(path.resolve(base, rel)); }
function walk(d, out) { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) walk(p, out); else if (/\.(h|hpp|inl|cpp)$/i.test(e.name)) out.push(p); } return out; }
// respell `rel` (relative to base) with the on-disk case of every component; keeps the relative shape
function respell(base, rel) {
  let cur = base; const out = [];
  for (const p of rel.split('/')) {
    if (p === '.' || p === '..' || p === '') { out.push(p); cur = path.resolve(cur, p); continue; }
    const l = list(cur); const hit = l && [...l].find(e => e.toLowerCase() === p.toLowerCase());
    if (!hit) return null;
    out.push(hit); cur = path.join(cur, hit);
  }
  return out.join('/');
}
const FIX = process.argv.includes('--fix');
const DIRS = process.argv.slice(2).filter(a => !a.startsWith('--'));
const files = (DIRS.length ? DIRS : ['xrGame', 'COMMON_AI']).flatMap(d => walk(path.join(ROOT, d), []));
let bad = 0;
for (const f of files) {
  const text = fs.readFileSync(f, 'utf8'); const nl = text.includes('\r\n') ? '\r\n' : '\n';
  const lines = text.split(/\r?\n/); let changed = false;
  lines.forEach((l, i) => {
    const m = /^(\s*#\s*include\s*")([^"]+)(".*)$/.exec(l); if (!m) return;
    const inc = m[2].replace(/\\/g, '/');
    const cands = [path.dirname(f), ...IDIRS];
    // MSVC picks the first candidate that exists ignoring case; GCC the first that exists exactly.
    // Anything else than "the same file" is a bug on Linux: either not found, or silently a DIFFERENT
    // header (e.g. "stdafx.h" from xrGame/ai/... finds xrCore/stdafx.h instead of xrGame/StdAfx.h).
    const loose = cands.find(c => existsAnyCase(c, inc));
    if (!loose) return;
    const exact = cands.find(c => existsExact(c, inc));
    if (exact && path.resolve(exact, inc).toLowerCase() === path.resolve(loose, inc).toLowerCase()) return;
    bad++;
    const fixed = respell(loose, inc);
    const why = exact ? `resolves to ${path.relative(ROOT, path.resolve(exact, inc))} instead of ${path.relative(ROOT, path.resolve(loose, inc))}` : `only matches ignoring case (under ${path.relative(ROOT, loose) || '.'})`;
    console.log(`${path.relative(ROOT, f)}:${i + 1}: "${m[2]}" ${why}${fixed ? ' -> "' + fixed + '"' : ''}`);
    if (FIX && fixed) { lines[i] = m[1] + fixed + m[3]; changed = true; }
  });
  if (changed) fs.writeFileSync(f, lines.join(nl));
}
console.log(bad ? `${bad} case mismatches${FIX ? ' (fixed)' : ''}` : 'all includes resolve with exact case');
