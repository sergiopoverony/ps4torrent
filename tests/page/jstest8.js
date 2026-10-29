// Плавность на странице: скорость подтягивается к новому значению, процент/полоса растут между опросами (предсказание), остаток времени
// уменьшается по секундам; подпись «входящие» в шапке сокращена.
const vm = require('vm'), fs = require('fs');
const script = fs.readFileSync('/tmp/page/script.js', 'utf8');
const dir = '/tmp/daemon/ps4torrentd/assets/lang/';
const results = []; const ok = (c, m) => { results.push(c); console.log((c ? 'PASS ' : 'FAIL ') + m); };
const GB = 1024 ** 3;
let nowMs = 1e6; const timers = [];
const els = {}; ['sum','m','msg','warn','cfg','up','cfgb','cfgs','mp','lp','cfgi','allgo','allstop','as','rs','ft','dlg','dfiles','dsel','dinfo','dwarn','dstart','dgo','dcancel','dlg2','f2title','f2info','f2warn','f2list','f2all','f2none','f2go','f2cancel','f2back','lng','quitb','urlb','ul','ulk','donb','support','supportImg','supportX']
  .forEach(k => els[k] = { style:{}, dataset:{}, textContent:'', value:'', innerHTML:'', files:[], checked:false, disabled:false, options:[], selectedIndex:0, querySelectorAll(){ return []; }, querySelector(){ return { textContent:'' }; }, scrollTop:0 });
// живые элементы списка: разбираем data-lv из текущего innerHTML
let live = [];
els.m.querySelectorAll = () => {
  live = [];
  const re = /<(\w+)([^>]*?)data-lv="([^"]+)"([^>]*)>(?:([^<]*)<\/\1>)?/g; let m;
  while ((m = re.exec(els.m.innerHTML))) {
    const key = m[3], o = { _k: key, style:{}, textContent: m[5] || '', getAttribute: n => n === 'data-lv' ? key : null };
    const w = /style="width:([\d.]+)%"/.exec(m[2] + m[4]); if (w) o.style.width = w[1] + '%';
    live.push(o);
  }
  return live;
};
const H = 'a'.repeat(40);
let status = { items: [{ hash:H, title:'Game', status:'downloading', size:1*GB, done:0.1*GB, pct:10, speed_kb:10000, eta_s:130, peers:5, paused:false, low_space:false, nfiles:1, nskip:0, root:'/mnt/usb0/torrents' }],
  drives:[], mounts:[], speed_kb:10000, listen_port:6881, incoming:184, deleting:0, version:'9.9.9.9' };
const ctx = { document:{ getElementById:id=>els[id], querySelectorAll:()=>[] }, location:{hash:''}, sessionStorage:{ getItem(){return null}, setItem(){} },
  localStorage:{ getItem(){return null}, setItem(){} }, navigator:{ language:'en', languages:['en'] }, prompt:()=>'', confirm:()=>true,
  setTimeout:(f)=>{ f && f(); return 0; }, clearTimeout:()=>{}, setInterval:(f, ms)=>{ timers.push({ f, ms }); return timers.length; }, Date:{ now:()=>nowMs },
  encodeURIComponent, decodeURIComponent, Array, String, Math, Promise, Error, parseInt, TextDecoder, Uint8Array, FileReader: function(){},
  fetch:(url)=>{
    if (url.startsWith('/lang/')) { const nm = url.slice(6).split('?')[0];
      const body = nm === 'index.json' ? fs.readdirSync(dir).filter(x => x.endsWith('.json')).map(x => ({ code: x.slice(0, -5), name: JSON.parse(fs.readFileSync(dir + x, 'utf8'))._name })) : JSON.parse(fs.readFileSync(dir + nm, 'utf8'));
      return Promise.resolve({ ok:true, status:200, json:()=>Promise.resolve(body) }); }
    const body = url.startsWith('/status') ? JSON.parse(JSON.stringify(status)) : { ok:true };
    return Promise.resolve({ ok:true, status:200, json:()=>Promise.resolve(body) }); } };
vm.createContext(ctx); vm.runInContext(script, ctx);
const tick = () => new Promise(r => setImmediate(r));
const flush = async () => { for (let i = 0; i < 10; i++) await tick(); };
const poll = async () => { live = []; timers.find(t => t.ms === 2000).f(); await flush(); live = []; };
const animate = () => timers.find(t => t.ms === 250).f();
const val = k => { if (!live.length) els.m.querySelectorAll(); return live.find(o => o._k === k + ':' + H); };
const num = s => parseFloat(/[\d.]+/.exec(s)[0]);

(async () => {
  await flush();
  ok(timers.some(t => t.ms === 250) && timers.some(t => t.ms === 2000), 'a 250 ms animation timer runs next to the 2 s poll');
  ok(els.sum.textContent.includes('peers: 184 (port 6881)') && !els.sum.textContent.includes('incoming'), 'header: "peers: 184 (port 6881)" without the word incoming: ' + els.sum.textContent);
  ok(val('spd').textContent.startsWith('10000 KB/s'), 'first sight: the real speed, no easing: ' + val('spd').textContent);
  ok(val('pct').textContent.startsWith('10%'), 'first sight: real percent ' + val('pct').textContent);

  // предсказание: между опросами процент, полоса и остаток времени идут вперёд
  nowMs += 1000; animate();
  const p1 = num(val('bar').style.width);
  ok(p1 > 10.5 && p1 < 11.5, 'after 1 s the bar moved forward by about speed*time (' + p1 + '%)');
  nowMs += 3000; animate();
  const p2 = num(val('bar').style.width);
  ok(p2 > p1, 'the bar keeps growing: ' + p1 + ' -> ' + p2);
  nowMs += 60000; animate();
  const p3 = num(val('bar').style.width);
  ok(p3 - p2 < 1.1, 'the prediction is capped (at most 5 s ahead): ' + p2 + ' -> ' + p3);

  // настоящий опрос: значение сверяется с консолью и не откатывается назад
  status.items[0].done = 0.2 * GB; status.items[0].pct = 20; nowMs += 100; await poll();
  ok(val('pct').textContent.startsWith('20%') || val('pct').textContent.startsWith('19%'), 'after a real poll the percent follows the console: ' + val('pct').textContent);
  const before = num(val('bar').style.width);
  nowMs += 1000; animate();
  ok(num(val('bar').style.width) >= before, 'never moves backward between polls');

  // остаток времени идёт по секундам
  status.items[0].eta_s = 130; nowMs += 100; await poll();
  ok(/ETA 2 min/.test(val('spd').textContent), 'ETA right after a poll: ' + val('spd').textContent);
  nowMs += 70000; animate();
  ok(/ETA 1 min/.test(val('spd').textContent), 'ETA counts down between polls (130 s -> 60 s): ' + val('spd').textContent);
  status.items[0].eta_s = 130; nowMs += 100; await poll();

  // скорость: плавный переход к новому значению
  status.items[0].speed_kb = 1000; status.speed_kb = 1000;
  nowMs += 100; await poll();
  const s0 = num(val('spd').textContent);
  nowMs += 300; animate(); const s1 = num(val('spd').textContent);
  nowMs += 600; animate(); const s2 = num(val('spd').textContent);
  nowMs += 3000; animate(); const s3 = num(val('spd').textContent);
  ok(s0 > s1 && s1 > s2 && s2 >= s3, 'speed glides to the new value instead of jumping: ' + [s0, s1, s2, s3].join(' > '));
  ok(s0 < 10000 && s3 > 0, 'it does not drop at once (poll-level smoothing + easing): ' + s0 + ' ... ' + s3);

  // не качается: никаких предсказаний
  status.items[0].status = 'paused'; status.items[0].paused = true; status.items[0].speed_kb = 0; status.speed_kb = 0; status.items[0].eta_s = -1;
  nowMs += 100; await poll(); const bp = val('bar').style.width;
  nowMs += 10000; animate();
  ok(val('bar').style.width === bp && val('spd').textContent.startsWith('0 KB/s'), 'a paused item does not move: ' + val('bar').style.width + ' ' + val('spd').textContent);

  // выкл. входящих
  status.listen_port = 0; nowMs += 100; await poll();
  ok(els.sum.textContent.includes('incoming: off'), 'listening off is shown as "incoming: off"');
  const p = results.filter(Boolean).length;
  console.log('RESULT: ' + p + '/' + results.length + ' passed'); process.exit(p === results.length ? 0 : 1);
})();
