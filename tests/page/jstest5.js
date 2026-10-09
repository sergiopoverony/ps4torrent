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
  ok(html.indexOf('id="dcancel"') < html.indexOf('id="dgo"'), 'in the first window Cancel comes first and Next/Download is to its right');
  // 1. одиночный файл: сразу Download
  await pick([['solo.torrent', single]]);
  ok(els.dgo.textContent === 'Download', 'a single-file torrent keeps the "Download" button');
  els.dgo.onclick(); await flush();
  ok(lastAdd() && !lastAdd().includes('skip=') && els.dlg2.style.display !== 'flex', 'it uploads at once, no second window, no skip: ' + lastAdd());
  // 2. несколько файлов: Next и окно 2
  calls.length = 0;
  await pick([['five.torrent', five]]);
  ok(els.dgo.textContent === 'Next', 'a multi-file torrent shows "Next"');
  els.dgo.onclick(); await flush();
  ok(els.dlg.style.display === 'none' && els.dlg2.style.display === 'flex' && calls.filter(c => c.startsWith('/api/add')).length === 0, 'Next opens the file window and uploads nothing yet');
  ok(els.f2list.innerHTML.includes('a.pkg') && els.f2list.innerHTML.includes('>e.pkg<') && els.f2list.innerHTML.includes('sub/') && (els.f2list.innerHTML.match(/checked/g) || []).length === 6, 'all 5 files are listed (folder sub/ as a block with its own checkbox) and ALL are checked by default');
  ok(els.f2info.textContent === 'Selected: 5 of 5 files, 8.0 GB' && els.f2go.disabled === false, 'summary: ' + els.f2info.textContent);
  ok(els.f2go.textContent === 'Start download' && html.includes('id="f2all">Select all<') && html.includes('id="f2none">Deselect all<') && html.includes('id="f2cancel" class="dng">Cancel<') && html.includes('id="f2go" class="go">Start download<') && html.includes('id="f2back">Back<'), 'the buttons are named as requested (Cancel red, Start download green, Back present)');
  // 3. снимаем файлы и запускаем
  chg(0, 1, false); chg(0, 2, false);
  ok(els.f2info.textContent === 'Selected: 3 of 5 files, 3.0 GB', 'unchecking updates the summary: ' + els.f2info.textContent);
  els.f2go.onclick(); await flush();
  ok(lastAdd() && lastAdd().includes('skip=1-2') && lastAdd().includes('drive=internal') && lastAdd().includes('start=1'), 'Start download sends the ranges of deselected files: ' + lastAdd());
  ok(els.dlg2.style.display === 'none' && els.dlg.style.display === 'none', 'both windows are closed');
  // 4. несколько диапазонов
  calls.length = 0;
  await pick([['five.torrent', five]]); els.dgo.onclick(); await flush();
  chg(0, 0, false); chg(0, 2, false); chg(0, 4, false);
  els.f2go.onclick(); await flush();
  ok(lastAdd().includes('skip=0%2C2%2C4'), 'separate files give a list: ' + lastAdd());
  // 5. выбрать всё / снять всё
  await pick([['five.torrent', five]]); els.dgo.onclick(); await flush();
  els.f2none.onclick(); await flush();
  ok(els.f2go.disabled === true && els.f2warn.textContent.includes('Select at least one file') && els.f2info.textContent.startsWith('Selected: 0 of 5'), 'Deselect all: Start is disabled with a hint');
  els.f2all.onclick(); await flush();
  ok(els.f2go.disabled === false && els.f2info.textContent.startsWith('Selected: 5 of 5'), 'Select all restores everything');
  // 6. место: выбрано больше, чем свободно
  els.f2cancel.onclick(); status.drives[0].free = 5 * GB;
  await pick([['five.torrent', five]]); els.dgo.onclick(); await flush();
  ok(els.f2warn.textContent.includes('Not enough free space') && els.f2warn.textContent.includes('8.0 GB'), 'the space warning uses the SELECTED size: ' + els.f2warn.textContent);
  chg(0, 2, false); chg(0, 1, false);
  ok(!els.f2warn.textContent.includes('Not enough'), 'after deselecting big files the warning goes away: "' + els.f2warn.textContent + '"');
  els.f2cancel.onclick(); status.drives[0].free = 40 * GB;
  ok(els.dlg.style.display === 'none' && els.dlg2.style.display === 'none', 'Cancel closes everything');
  // 7. несколько раздач
  calls.length = 0;
  await pick([['five.torrent', five], ['second.torrent', second], ['bad.torrent', junk]]);
  els.dgo.onclick(); await flush();
  ok(els.f2list.innerHTML.includes('Five Files') && els.f2list.innerHTML.includes('Second Pack') && !els.f2list.innerHTML.includes('bad.torrent'), 'several torrents: headers per torrent, the invalid file is not listed');
  els.f2list.onchange({ target: { dataset: { ga: '1' }, checked: false } }); await flush();
  ok(els.f2go.disabled === true && els.f2warn.textContent.includes('every torrent'), 'a torrent with nothing selected blocks Start: ' + els.f2warn.textContent);
  els.f2list.onchange({ target: { dataset: { ga: '1' }, checked: true } }); chg(0, 4, false); await flush();
  els.f2go.onclick(); await flush();
  const adds = calls.filter(c => c.startsWith('/api/add'));
  ok(adds.length === 2 && adds[0].includes('skip=4') && !adds[1].includes('skip='), 'each torrent gets its own skip list: ' + adds.map(a => a.slice(0, 80)).join(' | '));
  // 8. кнопка Files у задачи
  await flush();
  ok(els.m.innerHTML.includes('data-c="files" data-h="' + 'a'.repeat(40) + '"'), 'a multi-file unfinished task has a "files" button');
  ok(!els.m.innerHTML.includes('data-c="files" data-h="' + 'b'.repeat(40) + '"') && !els.m.innerHTML.includes('data-c="files" data-h="' + 'c'.repeat(40) + '"'), 'finished and single-file tasks have none');
  ok(els.m.innerHTML.includes('2/3 files'), 'the row shows how many files are selected: 2/3');
  calls.length = 0;
  els.m.onclick({ target: { dataset: { c: 'files', h: 'a'.repeat(40) }, disabled: false } }); await flush();
  ok(calls.some(c => c.startsWith('/api/files?hash=' + 'a'.repeat(40))), 'the button asks the console for the file list');
  ok(els.dlg2.style.display === 'flex' && els.f2title.textContent === 'Files: Pack One' && els.f2go.textContent === 'Apply', 'the window opens in "edit" mode: title and Apply button');
  ok(els.f2list.innerHTML.includes('>a.pkg<') && els.f2list.innerHTML.includes('dlc/') && els.f2list.innerHTML.includes('>b.pkg<') && !els.f2list.innerHTML.includes('Pack One/'), 'the common root folder is stripped from the paths');
  ok(els.f2info.textContent === 'Selected: 2 of 3 files, 3.0 GB', 'current selection shown: ' + els.f2info.textContent);
  chg(0, 1, true); ok(els.f2info.textContent.startsWith('Selected: 3 of 3'), 'switching a file on updates the summary');
  els.f2go.onclick(); await flush();
  const sel = calls.filter(c => c.startsWith('/api/select')).pop();
  ok(sel && sel.includes('hash=' + 'a'.repeat(40)) && sel.includes('skip=&') , 'Apply sends the (empty) skip list = everything selected: ' + sel);
  ok(els.dlg2.style.display === 'none', 'window closed after Apply');

  // 9. Сглаживание скорости на странице
  status.items[0].status = 'downloading'; status.items[0].speed_kb = 1000; status.speed_kb = 1000;
  ctx.load(true); await flush();
  ok(els.m.innerHTML.includes('1000 KB/s') && els.sum.textContent.startsWith('1000 KB/s'), 'the first reading is shown as it is');
  status.items[0].speed_kb = 200; status.speed_kb = 200;
  ctx.load(true); await flush();
  ok(els.m.innerHTML.includes('800 KB/s') && els.sum.textContent.startsWith('800 KB/s'), 'a sudden drop to 200 is shown softened (800 = 75% old + 25% new), per task and in the total: ' + els.sum.textContent.slice(0, 12));
  status.items[0].speed_kb = 200; status.speed_kb = 200;      // в реальности каждый ответ консоли новый объект
  ctx.load(true); await flush();
  ok(els.m.innerHTML.includes('650 KB/s'), 'and continues to follow gradually (650)');
  status.items[0].status = 'paused'; status.items[0].speed_kb = 0; status.speed_kb = 0;
  ctx.load(true); await flush();
  ok(els.m.innerHTML.includes('0 KB/s') && els.sum.textContent.startsWith('0 KB/s'), 'a stopped task shows 0 at once');
  console.log('RESULT: ' + results.filter(Boolean).length + '/' + results.length + ' passed');
  process.exit(0);
})();
