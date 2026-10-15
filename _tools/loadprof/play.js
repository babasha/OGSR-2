// Load cost vs the first seconds of PLAY. The [VSM prime] trace prints one line per
// frame; once loadscreen flips to 0 those lines are gameplay frames, so their deltas
// are a frame timer for exactly the window a shorter precache would damage.
const fs=require('fs');
const TS=/^\[\d\d\.\d\d\.\d\d (\d\d):(\d\d):(\d\d)\.(\d\d\d)\] \[(\d+)\] (.*)$/;
const PC=/\[precache\] (\d+)(?: of (\d+))? frames in (\d+) ms/;
console.log('run                  frames  precache  armed   playFrames  first50  p50  p90   max   >50ms');
for(const p of process.argv.slice(2)){
  const rows=[];
  for(const l of fs.readFileSync(p,'latin1').split(/\r?\n/)){const m=TS.exec(l);if(!m)continue;rows.push({t:((+m[1]*3600)+(+m[2]*60)+(+m[3]))*1000+(+m[4]),msg:m[6]});}
  const pc=rows.find(r=>PC.test(r.msg)); if(!pc) continue;
  const g=PC.exec(pc.msg);
  const frames=g[2]?`${g[1]}/${g[2]}`:g[1];
  const armed=rows.find(r=>/autopause armed after (\d+)/.test(r.msg));
  const down=rows.find(r=>/\[loadscreen\] down after (\d+)/.test(r.msg));
  const play=rows.filter(r=>/\[VSM prime\].*loadscreen=0/.test(r.msg)).map(r=>r.t);
  const d=[]; for(let i=1;i<play.length;i++) d.push(play[i]-play[i-1]);
  const s=[...d].sort((a,b)=>a-b), q=f=>s.length?s[Math.min(s.length-1,Math.floor(s.length*f))]:0;
  const name=p.split(/[\/]/).pop().replace(/^xray_[^_]+_/,'').replace('.log','');
  console.log(name.padEnd(18), frames.padStart(7),
    (g[3]+'ms').padStart(9),
    ((armed?/armed after (\d+)/.exec(armed.msg)[1]:(down?/down after (\d+)/.exec(down.msg)[1]:'-'))+'ms').padStart(8),
    String(play.length).padStart(10),
    (d.slice(0,50).reduce((a,b)=>a+b,0)+'ms').padStart(8),
    String(q(.5)).padStart(4), String(q(.9)).padStart(4), String(s.length?s[s.length-1]:0).padStart(5),
    String(d.filter(x=>x>50).length).padStart(6));
}
