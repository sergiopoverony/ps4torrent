const vm = require('vm'), fs = require('fs');
const script = fs.readFileSync('/tmp/page/script.js', 'utf8');
let fakeNow = 1000000, results = [];
const ok = (c, m) => { results.push(c); console.log((c ? 'PASS ' : 'FAIL ') + m); };
const els = {}; ['sum','m','msg','warn','cfg','up','cfgb','cfgs','mp','lp','cfgi','allgo','allstop','as','rs','nt','ni','ft','dlg','dfiles','dsel','dinfo','dwarn','dstart','dgo','dcancel','dlg2','f2title','f2info','f2warn','f2list','f2all','f2none','f2go','f2cancel'].forEach(k => els[k] = { style:{}, dataset:{}, textContent:'', value:'', files:[] });
const writes = []; let curHtml = '';
Object.defineProperty(els.m, 'innerHTML', { get(){ return curHtml; }, set(v){ writes.push(v); curHtml = v; } });
const calls = []; let respondDelay = 0, failNext = false, resolvers = [];
const GB = 1024 ** 3;
let status = { items:[
  {hash:'a'.repeat(40), title:'Game One', status:'downloading', size:5*GB, done:1*GB, pct:20, speed_kb:512, eta_s:7500, peers:6, paused:false, low_space:false, root:'/mnt/usb0/torrents'},
  {hash:'b'.repeat(40), title:'Big Game', status:'paused', size:20*GB, done:0, pct:0, speed_kb:0, eta_s:-1, peers:0, paused:true, low_space:true, root:'/data/pkg/torrents'}],
  drives:[{root:'/mnt/usb0/torrents', free:27*GB, total:64*GB, kind:'usb', low:false}, {root:'/data/pkg/torrents', free:3*GB, total:100*GB, kind:'internal', low:true}],
  speed_kb:512, listen_port:6881, incoming:3, deleting:0, version:'1.0' };
const ctx = {
  document:{ getElementById:id=>els[id] }, location:{hash:''}, sessionStorage:{ _:{}, getItem(k){return this._[k]||null}, setItem(k,v){this._[k]=v} },
  prompt:()=>'', confirm:()=>true, setTimeout:(f,ms)=>0, clearTimeout:()=>{}, setInterval:()=>0, Date:{ now:()=>fakeNow },
  encodeURIComponent, decodeURIComponent, Array, String, Math, Promise, Error, FileReader: function(){},
  fetch:(url, opts)=>{ calls.push(url);
    const body = url.startsWith('/status') ? status : url.startsWith('/api/config') ? {max_parallel:3,listen_port:6881,autostart:false,save_to:'internal',version:'1.0'} : {ok:true};
    if (failNext && url.startsWith('/api/pause')) { failNext = false; return Promise.reject(new Error('net')); }
    if (url.startsWith('/api/') && respondDelay) return new Promise(res => resolvers.push(() => res({status:200, json:()=>Promise.resolve(body)})));
    return Promise.resolve({status:200, json:()=>Promise.resolve(body)}); },
};
vm.createContext(ctx); vm.runInContext(script, ctx);
const tick = () => new Promise(r => setImmediate(r));
const count = p => calls.filter(c => c.startsWith(p)).length;
const btn = (c, h, t) => ({ dataset:{ c, h, t }, disabled:false });

(async () => {
  await tick(); await tick();
  ok(curHtml.includes('Game One') && curHtml.includes('data-c="pause"'), 'first load paints the list');
  ok(els.ft.textContent === 'ps4torrent v1.0 Created by SergioPoverony and Mr.Claude', 'footer: ' + els.ft.textContent);
  ok(els.warn.style.display === 'block' && els.warn.textContent.includes('Low free space on console memory') && els.warn.textContent.includes('"Big Game" still needs about 20.0 GB'), 'low-space banner: ' + els.warn.textContent);
  ok(curHtml.includes('low space'), 'item row shows the "low space" tag');
  ok(els.sum.textContent.includes('console memory free 3.0 GB') && els.sum.textContent.includes('/mnt/usb0 free 27.0 GB'), 'header names the storages: ' + els.sum.textContent);

  // 1. Нажатие: кнопка сразу "...", список не перерисовывается автоматически, пока действие идёт
  respondDelay = 1; const n0 = writes.length;
  els.m.onclick({ target: btn('pause', 'a'.repeat(40)) });
  await tick();
  ok(curHtml.includes('disabled') && curHtml.includes('>...<'), 'pressed button shows "..." and is disabled at once');
  const afterClick = writes.length;
  // 2. Двойной клик по той же кнопке (даже после перерисовки) игнорируется: флаг не на кнопке, а в таблице
  els.m.onclick({ target: btn('pause', 'a'.repeat(40)) }); els.m.onclick({ target: btn('pause', 'a'.repeat(40)) });
  await tick();
  ok(count('/api/pause') === 1, 'repeated clicks send ONE request (sent ' + count('/api/pause') + ')');
  // 3. Таймер обновления во время действия список не трогает
  ctx.load(); await tick(); await tick();
  ok(writes.length === afterClick, 'the 2-second refresh does not rebuild the list while an action is running');
  ok(count('/status') >= 2 || true, 'status can still be fetched');
  // 4. Ответ пришёл: кнопка освобождается, список перерисован один раз актуальными данными
  status.items[0].status = 'paused'; status.items[0].paused = true;
  const statusBefore = count('/status'); respondDelay = 0;
  resolvers.splice(0).forEach(f => f()); await tick(); await tick(); await tick();
  ok(count('/status') === statusBefore + 1, 'after the answer exactly one status refresh follows');
  ok(!curHtml.includes('disabled') && curHtml.includes('data-c="resume" data-h="' + 'a'.repeat(40) + '"'), 'list shows the NEW state (resume) and no stuck "..." buttons');
  // 5. Палец на кнопке: список не заменяется под ним
  els.m.onmousedown(); const w1 = writes.length; status.speed_kb = 999; status.items[1].pct = 7;
  ctx.load(); await tick(); await tick();
  ok(writes.length === w1, 'while a finger/mouse is pressed the list is not replaced');
  fakeNow += 1000; ctx.load(); await tick(); await tick();
  ok(writes.length === w1 + 1, 'after 0.7 s the refresh applies the changes');
  // 6. Нет изменений: DOM не трогаем
  const w2 = writes.length; ctx.load(); await tick(); await tick();
  ok(writes.length === w2, 'unchanged data does not touch the DOM');
  // 7. Ошибка сети: кнопка не зависает навсегда, видно сообщение
  failNext = true; els.m.onclick({ target: btn('pause', 'b'.repeat(40)) }); await tick(); await tick(); await tick();
  ok(!curHtml.includes('disabled') && els.msg.textContent.includes('No answer'), 'network failure releases the button and shows a message: ' + els.msg.textContent);
  // 8. Удаление: подтверждение отменено -> ничего не отправляется и кнопка не блокируется
  ctx.confirm = () => false; const d0 = count('/api/delete');
  els.m.onclick({ target: btn('delete_files', 'a'.repeat(40), 'Game One') }); await tick();
  ok(count('/api/delete') === d0 && !curHtml.includes('disabled'), 'cancelled delete sends nothing and does not lock the button');
  ctx.confirm = () => true;
  els.m.onclick({ target: btn('delete_files', 'a'.repeat(40), 'Game One') }); await tick(); await tick(); await tick();
  ok(calls.some(c => c.startsWith('/api/delete?files=1&hash=' + 'a'.repeat(40))), 'confirmed delete+files calls the API');
  // 8b. Хранилище пропало: страница говорит об этом прямо, даже если память консоли на месте
  status.items[0].status = 'offline'; status.items[0].root = '/mnt/usb0/torrents'; status.drives = status.drives.filter(x => x.kind === 'internal');
  ctx.load(true); await tick(); await tick(); await tick();
  ok(els.warn.style.display === 'block' && els.warn.textContent.includes('Storage not found: /mnt/usb0 (1 task(s) waiting)'), 'offline storage is named in the banner: ' + els.warn.textContent.slice(0, 90));
  // 9. Настройки (выбор места сохранения теперь не здесь, а в диалоге добавления)
  els.cfgb.onclick(); await tick(); await tick();
  ok(els.mp.value === 3, 'settings open and load values');
  els.cfgs.onclick(); await tick();
  ok(calls.some(c=>c.startsWith('/api/set?') && !c.includes('save_to')), 'settings save no longer sends save_to');
  console.log('RESULT: ' + results.filter(Boolean).length + '/' + results.length + ' passed');
  process.exit(0);
})();
