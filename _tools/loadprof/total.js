// End to end: from the `load` command to the moment the load is over (the screen
// coming down, or the autopause arming when keypress_on_start holds it).
const fs=require('fs');
const TS=/^\[\d\d\.\d\d\.\d\d (\d\d):(\d\d):(\d\d)\.(\d\d\d)\] \[(\d+)\] (.*)$/;
console.log('run                  total   alife  cform  render(4)                 spawn  sync   phases');
for(const p of process.argv.slice(2)){
  const rows=[];
  for(const l of fs.readFileSync(p,'latin1').split(/\r?\n/)){const m=TS.exec(l);if(!m)continue;rows.push({t:((+m[1]*3600)+(+m[2]*60)+(+m[3]))*1000+(+m[4]),msg:m[6]});}
  const cmd=rows.find(r=>/cl_console\] > load /.test(r.msg)); if(!cmd) continue;
  const end=rows.find(r=>/autopause armed after|\[loadscreen\] down after/.test(r.msg)); if(!end) continue;
  const ph=n=>{const r=rows.find(x=>x.msg.includes(`'${n}' = `)); return r?+/= (\d+)/.exec(r.msg)[1]:0;};
  const rend=rows.filter(r=>/'\(render\)' = \d+/.test(r.msg)).map(r=>+/= (\d+)/.exec(r.msg)[1]);
  const name=p.split(/[\/]/).pop().replace('xray_egorb_','').replace('.log','');
  console.log(name.padEnd(18), ((end.t-cmd.t)+'ms').padStart(7),
    (ph('st_loading_alife_simulator')+'').padStart(6), (ph('st_loading_cform')+'').padStart(6),
    ('['+rend.join(' ')+']').padStart(24), (ph('st_client_spawning')+'').padStart(6),
    (ph('st_client_synchronising')+'').padStart(5),
    ('= '+rend.reduce((a,b)=>a+b,0)+' render').padStart(14));
}
