const vm = require('vm'), fs = require('fs');
const script = fs.readFileSync('/tmp/page/script.js', 'utf8');
const html = fs.readFileSync('/tmp/page/page.html', 'utf8');
const results = []; const ok = (c, m) => { results.push(c); console.log((c ? 'PASS ' : 'FAIL ') + m); };
const GB = 1024 ** 3, enc = new TextEncoder();
function benc(x){
  if (typeof x === 'number') return enc.encode('i' + x + 'e');
  if (typeof x === 'string') { const b = enc.encode(x); return Buffer.concat([enc.encode(b.length + ':'), b]); }
  if (Buffer.isBuffer(x)) return Buffer.concat([enc.encode(x.length + ':'), x]);
  if (Array.isArray(x)) return Buffer.concat([enc.encode('l'), ...x.map(benc), enc.encode('e')]);
  const keys = Object.keys(x).sort();
  return Buffer.concat([enc.encode('d'), ...keys.map(k => Buffer.concat([benc(k), benc(x[k])])), enc.encode('e')]);
}
const ab = b => b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
const mk = (name, files) => ab(benc({ info: { name, files: files.map(([p, l]) => ({ length: l, path: p.split('/') })), 'piece length': 262144, pieces: Buffer.alloc(20 * 100, 1) } }));
const single = ab(benc({ info: { name: 'Solo Game', length: 5 * GB, 'piece length': 262144, pieces: Buffer.alloc(20 * 100, 1) } }));
const five = mk('Five Files', [['a.pkg', 1 * GB], ['b.pkg', 2 * GB], ['c.pkg', 3 * GB], ['d.pkg', 1 * GB], ['sub/e.pkg', 1 * GB]]);   // 8 ГБ
const second = mk('Second Pack', [['x.bin', 100], ['y.bin', 200]]);
const junk = new Uint8Array([1, 2, 3]).buffer;

const els = {}; ['sum','m','msg','warn','cfg','up','cfgb','cfgs','mp','lp','cfgi','allgo','allstop','as','rs','ft','dlg','dfiles','dsel','dinfo','dwarn','dstart','dgo','dcancel','dlg2','f2title','f2info','f2warn','f2list','f2all','f2none','f2go','f2cancel','f2back'].forEach(k => els[k] = { style:{}, dataset:{}, textContent:'', value:'', innerHTML:'', files:[], checked:false, disabled:false, options:[], selectedIndex:0, querySelectorAll(){ return []; }, scrollTop:0 });
Object.defineProperty(els.dsel, 'innerHTML', { get(){ return this._h || ''; }, set(v){ this._h = v; const re = /<option value="([^"]*)">([^<]*)<\/option>/g; let m; this.options = []; while ((m = re.exec(v))) this.options.push({ value: m[1], text: m[2].replace(/&quot;/g,'"').replace(/&amp;/g,'&') }); } });
let status = { items: [
  { hash:'a'.repeat(40), title:'Pack One', status:'paused', size:5*GB, done:0, pct:0, speed_kb:0, eta_s:-1, peers:0, paused:true, low_space:false, nfiles:3, nskip:1, root:'/mnt/usb0/torrents' },
  { hash:'b'.repeat(40), title:'Finished', status:'complete', size:5*GB, done:5*GB, pct:100, speed_kb:0, eta_s:0, peers:0, paused:false, low_space:false, nfiles:4, nskip:0, root:'/mnt/usb0/torrents' },
  { hash:'c'.repeat(40), title:'One File', status:'paused', size:1*GB, done:0, pct:0, speed_kb:0, eta_s:-1, peers:0, paused:true, low_space:false, nfiles:1, nskip:0, root:'/mnt/usb0/torrents' }],
  drives:[{ root:'/data/pkg/torrents', free:40*GB, total:100*GB, kind:'internal', low:false }],
  mounts:[{ n:0, mount:'/mnt/usb0', has_torrents:true, writable:true, free:27*GB, total:64*GB, low:false }],
  speed_kb:0, listen_port:6881, incoming:0, deleting:0, version:'1.3.0.0' };
const calls = [], bodies = [];
const filesReply = { ok:true, hash:'a'.repeat(40), title:'Pack One', complete:false, files:[{ i:0, size:1*GB, sel:true, path:'Pack One/a.pkg' }, { i:1, size:2*GB, sel:false, path:'Pack One/dlc/b.pkg' }, { i:2, size:2*GB, sel:true, path:'Pack One/c.pkg' }] };
const ctx = { document:{ getElementById:id=>els[id] }, location:{hash:''}, sessionStorage:{ getItem(){return null}, setItem(){} }, prompt:()=>'', confirm:()=>true,
  setTimeout:(f)=>{ f && f(); return 0; }, clearTimeout:()=>{}, setInterval:()=>0, Date:{ now:()=>1e6 }, encodeURIComponent, decodeURIComponent, Array, String, Math, Promise, Error, parseInt, TextDecoder, Uint8Array,
  FileReader: function(){ this.readAsArrayBuffer = function(f){ this.result = f.__buf; setImmediate(() => this.onload()); }; },
  fetch:(url, opts)=>{ calls.push(url); bodies.push(opts && opts.body);
    const body = url.startsWith('/status') ? status : url.startsWith('/api/files') ? filesReply : { ok:true };
    return Promise.resolve({ status:200, json:()=>Promise.resolve(body) }); } };
vm.createContext(ctx); vm.runInContext(script, ctx);
const tick = () => new Promise(r => setImmediate(r));
const flush = async () => { for (let i = 0; i < 8; i++) await tick(); };
const pick = async (list) => { els.up.files = list.map(([name, buf]) => ({ name, __buf: buf })); els.up.onchange.call(els.up); await flush(); };
const chg = (g, f, v) => els.f2list.onchange({ target: { dataset: { g: String(g), f: String(f) }, checked: v } });
const lastAdd = () => calls.filter(c => c.startsWith('/api/add')).pop();

(async () => {
  await flush();
  // GoW-подобная раздача: pkg из корня вперемешку с файлами папки .pad и ещё одна папка
  const gow = mk('God Of War', [['2_B.pkg', 7 * GB], ['.pad/1900544', 2e6], ['1_A.pkg', 35 * GB], ['.pad/1638400', 1.6e6], ['EP9000-X.pkg', 1e6], ['.pad/3145728', 3e6], ['dlc/z.bin', 100], ['10_C.pkg', 5 * GB]]);
  await pick([['gow.torrent', gow]]);
  els.dgo.onclick(); await flush();
  const h = els.f2list.innerHTML;
  const pos = s => h.indexOf(s);
  ok(pos('>1_A.pkg<') < pos('>2_B.pkg<') && pos('>2_B.pkg<') < pos('>10_C.pkg<') && pos('>10_C.pkg<') < pos('>EP9000-X.pkg<'), 'root files come first, in natural order (1, 2, 10, EP)');
  ok(pos('>EP9000-X.pkg<') < pos('.pad/') && pos('.pad/') < pos('>1638400<') && pos('>1638400<') < pos('>1900544<') && pos('>1900544<') < pos('>3145728<'), 'the .pad folder is a block AFTER all root files, its files sorted');
  ok(pos('>3145728<') < pos('dlc/') && pos('dlc/') < pos('>z.bin<'), 'next folder follows as its own block');
  ok(!h.includes('.pad/1638400'), 'files inside a folder show only their short name');
  ok((h.match(/data-gd=/g) || []).length === 2, 'each folder has its own checkbox');
  ok(els.f2info.textContent.startsWith('Selected: 8 of 8 files'), 'summary counts all 8 files: ' + els.f2info.textContent);
  // снять папку целиком
  const dirBox = (name, v) => els.f2list.onchange({ target: { dataset: { gd: '0', dn: name }, checked: v } });
  dirBox('.pad', false);
  ok(els.f2info.textContent.startsWith('Selected: 5 of 8 files'), 'unchecking the folder .pad unchecks its 3 files: ' + els.f2info.textContent);
  ok(els.f2list.innerHTML.includes('data-dn=".pad"><b>') || !/data-dn="\.pad"[^>]* checked/.test(els.f2list.innerHTML), 'the folder checkbox is unchecked');
  // выбрать один файл папки: чекбокс папки частичный
  els.f2list.onchange({ target: { dataset: { g: '0', f: '1' }, checked: true } });
  ok(/data-dn="\.pad" data-part="1"/.test(els.f2list.innerHTML) && els.f2info.textContent.startsWith('Selected: 6 of 8 files'), 'one file of the folder: the folder checkbox is partial');
  // Back: выбор не теряется
  els.f2back.onclick(); await flush();
  ok(els.dlg.style.display === 'flex' && els.dlg2.style.display === 'none' && calls.filter(c => c.startsWith('/api/add')).length === 0, 'Back returns to the first window and uploads nothing');
  els.dgo.onclick(); await flush();
  ok(els.dlg2.style.display === 'flex' && els.f2info.textContent.startsWith('Selected: 6 of 8 files'), 'Next again: the earlier selection is kept: ' + els.f2info.textContent);
  ok(els.f2back.style.display === '', 'Back is visible when adding');
  els.f2go.onclick(); await flush();
  ok(lastAdd() && /skip=/.test(lastAdd()), 'Start download sends the skip list: ' + lastAdd());
  // Back не нужен в окне "files" уже идущей задачи
  els.m.onclick({ target: { dataset: { c: 'files', h: 'a'.repeat(40) }, disabled: false } }); await flush();
  ok(els.f2back.style.display === 'none', 'Back is hidden in the Files window of an existing task');
  els.f2cancel.onclick();
  // дубликат
  calls.length = 0;
  ctx.fetch = (url, opts) => { calls.push(url); return Promise.resolve({ status: 200, json: () => Promise.resolve(url.startsWith('/api/add') ? { ok: true, duplicate: true, title: 'Solo Game' } : url.startsWith('/status') ? status : { ok: true }) }); };
  await pick([['solo.torrent', single]]);
  els.dgo.onclick(); await flush();
  ok(/Already in the list, not added again: Solo Game/.test(els.msg.textContent), 'a duplicate is reported on the page: ' + els.msg.textContent);
  ok(!/^Added/.test(els.msg.textContent), 'and it is not reported as added');
  const bad = results.filter(x => !x).length;
  console.log('RESULT: ' + (results.length - bad) + '/' + results.length + ' passed');
})().catch(e => { console.log('EXC ' + e.stack); });
