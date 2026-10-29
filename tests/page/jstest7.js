// Языки и кнопка выключения: автоопределение по браузеру, ручной выбор, полнота языковых файлов, метка паузы из-за памяти.
const vm = require('vm'), fs = require('fs');
const script = fs.readFileSync('/tmp/page/script.js', 'utf8');
const html = fs.readFileSync('/tmp/page/page.html', 'utf8');
const dir = '/tmp/daemon/ps4torrentd/assets/lang/';
const results = []; const ok = (c, m) => { results.push(c); console.log((c ? 'PASS ' : 'FAIL ') + m); };
const GB = 1024 ** 3;
const en = JSON.parse(fs.readFileSync(dir + 'en.json', 'utf8')), ru = JSON.parse(fs.readFileSync(dir + 'ru.json', 'utf8'));

// ---- статическая проверка файлов
ok(JSON.stringify(Object.keys(en).sort()) === JSON.stringify(Object.keys(ru).sort()), 'en.json and ru.json have the same keys');
const ph = s => (String(s).match(/\{\d\}/g) || []).sort().join('');
ok(Object.keys(en).every(k => ph(en[k]) === ph(ru[k])), 'every key has the same {0},{1} placeholders in both languages: ' + Object.keys(en).filter(k => ph(en[k]) !== ph(ru[k])).join(','));
const used = new Set();
(script + html).replace(/\bT\('([a-z0-9_]+)'/g, (m, k) => { if (!k.endsWith('_')) used.add(k); });
['u_B', 'u_KB', 'u_MB', 'u_GB', 'u_TB'].forEach(k => used.add(k));      // sz(): T('u_' + ...) собирается списком
html.replace(/data-i(?:18n|title)="([a-z0-9_]+)"/g, (m, k) => used.add(k));
['st_downloading', 'st_paused', 'st_complete', 'st_offline', 'st_checking', 'st_waiting', 'st_queued'].forEach(k => used.add(k));   // T('st_' + status)
const missing = [...used].filter(k => !(k in en));
ok(missing.length === 0, 'every text key used by the page exists in en.json: ' + missing.join(','));
['st_error'].forEach(k => used.add(k));
// Ключи tc_* и err_* читает приложение на консоли (общие языковые файлы с 1.8.0.3), страница их не использует.
const unused = Object.keys(en).filter(k => !used.has(k) && k !== '_name' && !/^(tc_|err_)/.test(k));
ok(unused.length === 0, 'no unused keys in en.json: ' + unused.join(','));

function mkctx(nav, stored, confirmAns){
  const els = {}; ['sum','m','msg','warn','cfg','up','cfgb','cfgs','mp','lp','cfgi','allgo','allstop','as','rs','ft','dlg','dfiles','dsel','dinfo','dwarn','dstart','dgo','dcancel','dlg2','f2title','f2info','f2warn','f2list','f2all','f2none','f2go','f2cancel','f2back','lng','quitb','urlb','ul','ulk','donb','support','supportImg','supportX']
    .forEach(k => els[k] = { style:{}, dataset:{}, textContent:'', value:'', innerHTML:'', files:[], checked:false, disabled:false, options:[], selectedIndex:0, querySelectorAll(){ return []; }, querySelector(){ return { textContent:'' }; }, scrollTop:0 });
  const store = Object.assign({}, stored), calls = [];
  const stat = []; html.replace(/<(\w+)([^>]*?)data-i18n="([^"]+)"([^>]*)>/g, (m, tag, a, key, b) => {
    const idm = (a + b).match(/id="([^"]+)"/), tm = (a + b).match(/data-ititle="([^"]+)"/), el = idm ? els[idm[1]] : { };
    stat.push({ getAttribute: n => n === 'data-i18n' ? key : n === 'data-ititle' ? (tm ? tm[1] : null) : null,
      set textContent(v){ el.textContent = v; }, get textContent(){ return el.textContent; }, set title(v){ el.title = v; } });
  });
  const status = { items: [
    { hash:'a'.repeat(40), title:'Slow One', status:'paused', mem_paused:true, size:5*GB, done:0, pct:10, speed_kb:0, eta_s:-1, peers:0, paused:true, low_space:false, nfiles:1, nskip:0, root:'/mnt/usb0/torrents' },
    { hash:'b'.repeat(40), title:'Fast One', status:'downloading', size:2*GB, done:GB, pct:50, speed_kb:900, eta_s:3700, peers:7, paused:false, low_space:false, nfiles:1, nskip:0, root:'/mnt/usb0/torrents' }],
    drives:[{ root:'/data/pkg/torrents', free:40*GB, total:100*GB, kind:'internal', low:false }], mounts:[],
    speed_kb:900, listen_port:0, incoming:0, deleting:0, version:'9.9.9.9' };
  const ctx = { document:{ getElementById:id=>els[id], querySelectorAll:()=>stat }, location:{hash:''}, sessionStorage:{ getItem(){return null}, setItem(){} },
    localStorage:{ getItem:k=>(k in store ? store[k] : null), setItem:(k,v)=>{ store[k] = v; } }, navigator: nav,
    prompt:()=>'', confirm:()=>confirmAns, setTimeout:(f)=>{ f && f(); return 0; }, clearTimeout:()=>{}, setInterval:()=>0, Date:{ now:()=>1e6 },
    encodeURIComponent, decodeURIComponent, Array, String, Math, Promise, Error, parseInt, TextDecoder, Uint8Array, FileReader: function(){},
    fetch:(url)=>{ calls.push(url);
      if (url.startsWith('/lang/')) { const nm = url.slice(6).split('?')[0];
        if (nm !== 'index.json' && !fs.existsSync(dir + nm)) return Promise.resolve({ ok:false, status:404, json:()=>Promise.reject(new Error('x')) });
        const body = nm === 'index.json' ? fs.readdirSync(dir).filter(x => x.endsWith('.json')).map(x => ({ code: x.slice(0, -5), name: JSON.parse(fs.readFileSync(dir + x, 'utf8'))._name })) : JSON.parse(fs.readFileSync(dir + nm, 'utf8'));
        return Promise.resolve({ ok:true, status:200, json:()=>Promise.resolve(body) }); }
      const body = url.startsWith('/status') ? status : url.startsWith('/api/config') ? { max_parallel:3, listen_port:0, autostart:true, resume_session:true, version:'9.9.9.9' } : { ok:true };
      return Promise.resolve({ ok:true, status:200, json:()=>Promise.resolve(body) }); } };
  vm.createContext(ctx); vm.runInContext(script, ctx);
  return { ctx, els, store, calls };
}
const tick = () => new Promise(r => setImmediate(r));
const flush = async () => { for (let i = 0; i < 10; i++) await tick(); };

(async () => {
  let t = mkctx({ language:'ru-RU', languages:['ru-RU','en-US'] }, {}, true); await flush();
  ok(t.els.cfgb.textContent === ru.btn_settings && t.els.quitb.textContent === ru.btn_quit, 'browser language ru-RU -> Russian static texts');
  ok(t.els.m.innerHTML.includes('Slow One') && t.els.m.innerHTML.includes(ru.st_mem_paused) && t.els.m.innerHTML.includes('КБ/с'), 'rows are Russian, memory-paused label shown: ' + (t.els.m.innerHTML.match(/<span class="s">[^<]*/g) || []).join('|'));
  ok(t.els.warn.textContent.includes('Мало памяти на консоли'), 'warning about automatically paused downloads: ' + t.els.warn.textContent);
  ok(t.els.sum.textContent.includes('КБ/с') && t.els.ft.textContent === 'ps4torrent v9.9.9.9 Created by SergioPoverony and Mr.Claude', 'summary Russian, footer credit unchanged');
  ok(t.els.m.innerHTML.includes('1 ч 2 мин') || t.els.m.innerHTML.includes('осталось'), 'time/ETA Russian');

  t = mkctx({ language:'en-GB', languages:['en-GB'] }, {}, true); await flush();
  ok(t.els.cfgb.textContent === 'settings' && t.els.m.innerHTML.includes('paused: low memory') && t.els.m.innerHTML.includes('KB/s'), 'browser language en-GB -> English');
  t = mkctx({ language:'de-DE', languages:['de-DE'] }, {}, true); await flush();
  ok(t.els.cfgb.textContent === 'settings', 'unknown language de -> English');
  t = mkctx({ language:'ru', languages:['ru'] }, { lang:'en' }, true); await flush();
  ok(t.els.cfgb.textContent === 'settings', 'manual choice (en) wins over the browser language');
  t = mkctx({ language:'en', languages:['en'] }, { lang:'xx' }, true); await flush();
  ok(t.els.cfgb.textContent === 'settings', 'a stored language that does not exist falls back to the browser/English');

  // выбор в настройках
  t = mkctx({ language:'en', languages:['en'] }, {}, true); await flush();
  t.els.cfgb.onclick(); await flush();
  const opts = t.els.lng.innerHTML;
  ok(opts.includes('value="auto"') && opts.includes('>English<') && opts.includes('>Русский<'), 'settings has a language list (auto, English, Русский)');
  t.els.lng.value = 'ru'; t.els.lng.onchange(); await flush();
  ok(t.store.lang === 'ru' && t.els.cfgb.textContent === ru.btn_settings && t.els.m.innerHTML.includes(ru.b_resume), 'switching to Russian: saved, texts and rows change at once');
  t.els.lng.value = 'auto'; t.els.lng.onchange(); await flush();
  ok(t.store.lang === 'auto' && t.els.cfgb.textContent === 'settings', 'switching back to auto (browser = English)');

  // кнопка выключения
  t = mkctx({ language:'en', languages:['en'] }, {}, false); await flush();
  ok(/id="quitb" class="dng"/.test(html) && html.indexOf('id="cfgb"') < html.indexOf('id="quitb"') && html.indexOf('id="quitb"') < html.indexOf('</header>'), 'the red shut-down button is in the header to the right of Settings');
  t.calls.length = 0; t.els.quitb.onclick(); await flush();
  ok(!t.calls.some(c => c.startsWith('/quit')), 'confirmation declined: nothing is sent');
  t = mkctx({ language:'en', languages:['en'] }, {}, true); await flush();
  t.calls.length = 0; t.els.quitb.onclick(); await flush();
  ok(t.calls.some(c => c.startsWith('/quit')), 'confirmed: /quit is called');
  ok(t.els.msg.textContent === en.quit_done, 'after shutting down the page says how to start it again');

  const p = results.filter(Boolean).length;
  console.log('RESULT: ' + p + '/' + results.length + ' passed'); process.exit(p === results.length ? 0 : 1);
})();
