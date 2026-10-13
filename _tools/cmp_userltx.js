// Compare every r_* line in user.ltx against the compiled default in
// vk_console_min.cpp. Recorded technique (gate-cvars-must-not-be-savable):
// user.ltx is rewritten WHOLE from current values, so a line that MATCHES the
// code means nobody touched it. The lines that differ are the stuck ones.
const fs = require('fs');
const src = fs.readFileSync('D:/OGSR-ONLINE/ogsr_engine/Layers/xrRenderVulkan/vk_console_min.cpp', 'utf8');
const ltx = fs.readFileSync('D:/OGSR Gunslinger Addon/_appdata_/user.ltx', 'utf8');

// defaults:  int ps_r_foo = 3;   /  float ps_r_foo = 0.5f;
const defs = {};
for (const m of src.matchAll(/^\s*(?:int|float)\s+(ps_r_[a-z0-9_]+)\s*=\s*([-0-9.]+)f?\s*;/gm))
    defs[m[1]] = parseFloat(m[2]);
// registrations: CMD4(CCC_XxxNoSave?, "r_foo", &ps_r_foo, ...)
const reg = {}, saveable = {};
for (const m of src.matchAll(/CMD4\(\s*(CCC_[A-Za-z]+)\s*,\s*"([a-z0-9_]+)"\s*,\s*&(ps_[a-z0-9_]+)/g)) {
    reg[m[2]] = m[3];
    saveable[m[2]] = !/NoSave/.test(m[1]);
}

const rows = [];
for (const line of ltx.split(/\r?\n/)) {
    const m = line.match(/^([a-z0-9_]+)\s+(.+?)\s*$/);
    if (!m) continue;
    const name = m[1];
    if (!name.startsWith('r_')) continue;
    if (!(name in reg)) continue;
    const v = parseFloat(m[2]);
    const d = defs[reg[name]];
    if (d === undefined || isNaN(v)) continue;
    if (Math.abs(v - d) > 1e-6) rows.push([name, m[2], d, saveable[name]]);
}

console.log('РАСХОЖДЕНИЯ user.ltx vs дефолт кода (' + rows.length + '):');
console.log('  ' + 'квар'.padEnd(26) + 'в файле'.padEnd(12) + 'дефолт'.padEnd(12) + 'сохраняемый?');
for (const r of rows)
    console.log('  ' + r[0].padEnd(26) + String(r[1]).padEnd(12) + String(r[2]).padEnd(12) + (r[3] ? 'ДА  <- застрянет снова' : 'нет'));

const stuck = rows.filter(r => String(r[0]).startsWith('r_gather')).length;
console.log('\nиз них по гейзеру: ' + stuck);
