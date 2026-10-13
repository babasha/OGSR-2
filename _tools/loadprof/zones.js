// Average the GPU zone table over the precache frames: what IS the 50 ms a frame?
const fs=require('fs');
const TS=/^\[\d\d\.\d\d\.\d\d (\d\d):(\d\d):(\d\d)\.(\d\d\d)\] \[(\d+)\] (.*)$/;
const rows=[];
for(const l of fs.readFileSync(process.argv[2],'latin1').split(/\r?\n/)){const m=TS.exec(l);if(!m)continue;rows.push({t:((+m[1]*3600)+(+m[2]*60)+(+m[3]))*1000+(+m[4]),msg:m[6]});}
const marks=rows.filter(r=>/^\[VK Perf\] MARK cpu=/.test(r.msg));
const skip=+(process.argv[3]||5);
const use=marks.slice(skip);            // drop the first few: they carry the load frame
console.log('precache MARK frames:', marks.length, '| averaging over', use.length);
const acc=new Map(), gpuTot=[];
for(const m of use){
  const g=/gpu_total=([\d.]+)ms/.exec(m.msg); if(g) gpuTot.push(+g[1]);
  for(const mm of m.msg.matchAll(/([A-Za-z0-9_\/]+)=([\d.]+)\(/g)) acc.set(mm[1],(acc.get(mm[1])||0)+ +mm[2]);
}
const n=use.length||1;
console.log('gpu_total avg', (gpuTot.reduce((a,b)=>a+b,0)/(gpuTot.length||1)).toFixed(2), 'ms  max', Math.max(...gpuTot).toFixed(2));
[...acc].map(([k,v])=>[k,v/n]).filter(([,v])=>v>=0.15).sort((a,b)=>b[1]-a[1]).slice(0,28)
  .forEach(([k,v])=>console.log('  '+k.padEnd(22), v.toFixed(2)+' ms'));
