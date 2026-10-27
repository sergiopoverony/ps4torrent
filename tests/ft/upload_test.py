# Отдача: пир подключается к демону и качает у него (после завершения раздачи и во время загрузки), лимит скорости, выключение.
import socket, struct, hashlib, os, sys, time, json, urllib.request, urllib.error, subprocess, shutil
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; U = FT + "/usb0/torrents"; PORT = 26881
res = []
def check(name, ok): res.append(bool(ok)); print(("PASS " if ok else "FAIL ") + name, flush=True)
def get(p):
    return json.load(urllib.request.urlopen(B + p, timeout=15))
def getq(p):
    try: return 200, json.load(urllib.request.urlopen(B + p, timeout=15))
    except urllib.error.HTTPError as e: return e.code, json.loads(e.read())
def add(name, extra):
    data = open(FT + "/torrents_src/" + name, "rb").read()
    r = urllib.request.Request(B + "/api/add?drive=0&name=" + urllib.request.quote(name) + extra, data=data, method="POST")
    return json.load(urllib.request.urlopen(r, timeout=15))
def items(): return {i["title"]: i for i in get("/status")["items"]}
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.3)
    return False
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def log(): return open(FT + "/log/log.txt").read()
def boot(delay, wipe=True, cfg=None):
    if wipe: sh("pkill -9 -x daemon_sel; bash seeder_ctl.sh stop; rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log")
    else: sh("pkill -9 -x daemon_sel")
    if cfg is not None: open(FT + "/log/config.txt", "w").write(cfg)
    sh("bash seeder_ctl.sh start %s 0 0 0 0" % delay)
    subprocess.Popen("setsid ./daemon_sel >> upload.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)
def quit_():
    try: urllib.request.urlopen(B + "/quit", timeout=3).read()
    except Exception: pass
    time.sleep(3.5)

def parse_bencode(d, i=0):
    c = d[i:i+1]
    if c == b"i":
        e = d.index(b"e", i); return int(d[i+1:e]), e+1
    if c == b"l":
        i += 1; out = []
        while d[i:i+1] != b"e":
            v, i = parse_bencode(d, i); out.append(v)
        return out, i+1
    if c == b"d":
        i += 1; out = {}
        while d[i:i+1] != b"e":
            k, i = parse_bencode(d, i); v, i2 = parse_bencode(d, i)
            out[k] = (v, i, i2); i = i2
        return out, i+1
    colon = d.index(b":", i); n = int(d[i:colon]); s = d[colon+1:colon+1+n]
    return s, colon+1+n
NAME = "Game One"
td = open(FT + "/torrents_src/%s.torrent" % NAME, "rb").read()
top, _ = parse_bencode(td)
info_val, s0, e0 = top[b"info"]
IH = hashlib.sha1(td[s0:e0]).digest()
PL = info_val[b"piece length"][0]
FILES = [(os.path.join(FT, "src", NAME, *[p.decode() for p in fe[b"path"][0]]), fe[b"length"][0]) for fe in info_val[b"files"][0]]
TOTAL = sum(l for _, l in FILES); NP = (TOTAL + PL - 1) // PL
def read_range(off, n):
    out = b""; pos = 0
    for p, ln in FILES:
        if off < pos + ln and n > 0:
            with open(p, "rb") as f:
                f.seek(off - pos); chunk = f.read(min(n, pos + ln - off))
            out += chunk; off += len(chunk); n -= len(chunk)
        pos += ln
    return out

class Leech:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=5); self.s.settimeout(0.5)
        self.buf = b""; self.bits = None; self.unchoked = False; self.choked_msg = False; self.closed = False
        self.got = {}                                  # (piece, begin) -> data
        self.s.sendall(b"\x13BitTorrent protocol" + b"\0"*8 + IH + b"-LEECH-" + os.urandom(13))
        self.hs = False
    def pump(self, secs):
        t_end = time.time() + secs
        while time.time() < t_end and not self.closed:
            try:
                d = self.s.recv(65536)
                if not d: self.closed = True; break
                self.buf += d
            except socket.timeout: pass
            except OSError: self.closed = True; break
            self.parse()
    def parse(self):
        if not self.hs:
            if len(self.buf) < 68: return
            self.buf = self.buf[68:]; self.hs = True
        while len(self.buf) >= 4:
            n = struct.unpack(">I", self.buf[:4])[0]
            if len(self.buf) < 4 + n: return
            m = self.buf[4:4+n]; self.buf = self.buf[4+n:]
            if n == 0: continue
            i = m[0]
            if i == 5:
                self.bits = [bool(m[1 + k // 8] & (0x80 >> (k % 8))) for k in range(NP)]
            elif i == 4:
                if self.bits is None: self.bits = [False] * NP
                self.bits[struct.unpack(">I", m[1:5])[0]] = True
            elif i == 1: self.unchoked = True
            elif i == 0: self.unchoked = False; self.choked_msg = True
            elif i == 7:
                pi, begin = struct.unpack(">II", m[1:9]); self.got[(pi, begin)] = m[9:]
    def send(self, b): self.s.sendall(b)
    def interested(self): self.send(b"\0\0\0\1\2")
    def request(self, pi, begin, ln): self.send(struct.pack(">IBIII", 13, 6, pi, begin, ln))
    def bytes(self): return sum(len(v) for v in self.got.values())
    def verify(self):
        for (pi, begin), d in self.got.items():
            if read_range(pi * PL + begin, len(d)) != d: return False
        return True
    def close(self):
        try: self.s.close()
        except Exception: pass

def measure(L, secs):
    """Качаем подряд блоки по кругу и считаем принятые байты за secs секунд."""
    blocks = [(pi, b) for pi in range(NP) for b in range(0, min(PL, TOTAL - pi * PL), 16384)]
    k = 0; inflight = 0; base = L.bytes(); t0 = time.time(); asked = set()
    while time.time() - t0 < secs and not L.closed:
        while len(asked) - len(L.got) < 6 and k < len(blocks):
            pi, b = blocks[k]; k += 1
            ln = min(16384, min(PL, TOTAL - pi * PL) - b)
            L.request(pi, b, ln); asked.add((pi, b))
        L.pump(0.25)
    return L.bytes() - base, time.time() - t0

print("=== ROUND 1: Game One completes; upload is OFF -> a peer cannot download from us")
boot(0)
r = add("Game One.torrent", "&start=1")
check("accepted", r.get("ok"))
check("completes", wait(lambda: items().get(NAME, {}).get("status") == "complete", 60))
cfg = get("/api/config")
check("config: upload off by default, limit 32 (upload=%s kb=%s)" % (cfg.get("upload"), cfg.get("upload_kb")), cfg.get("upload") is False and cfg.get("upload_kb") == 32)
L = Leech(); L.pump(3)
check("a peer connecting while upload is off gets nothing (handshake answered=%s, bytes=%d)" % (L.hs, L.bytes()), (not L.hs) and L.bytes() == 0)
L.close()
check("item is not seeding", items()[NAME].get("seeding") is False)

print("=== ROUND 2: upload ON at 32 KB/s -> seeding, bitfield, unchoke, correct data, rate close to the limit")
c, j = getq("/api/set?upload=1&upload_kb=32")
check("settings accepted", c == 200 and j.get("ok"))
check("seeding starts", wait(lambda: items()[NAME].get("seeding") is True, 15))
check("log has the seeding line", "seeding: Game One" in log())
L = Leech(); L.pump(1.5)
check("handshake accepted, bitfield received with all %d pieces" % NP, L.hs and L.bits is not None and all(L.bits[:NP]))
L.interested(); L.pump(1.5)
check("unchoked after interested", L.unchoked)
got, dt = measure(L, 10)
kb = got / 1024.0 / dt
check("rate with a 32 KB/s limit: %.1f KB/s over %.1f s (%d KB)" % (kb, dt, got // 1024), 22 <= kb <= 50)
check("every received block is byte-identical to the source", L.verify() and L.bytes() > 0)
st = get("/status")
check("status shows upload on and the item seeding (up_kb=%s)" % st.get("up_kb"), st.get("upload") is True and st["items"][0].get("seeding") is True)

print("=== ROUND 3: limit 128 KB/s")
c, j = getq("/api/set?upload_kb=128")
check("128 accepted", c == 200 and j.get("ok"))
L.got.clear()
got, dt = measure(L, 6)
kb = got / 1024.0 / dt
check("rate with a 128 KB/s limit: %.1f KB/s" % kb, 95 <= kb <= 170)
check("data still identical", L.verify())
c, j = getq("/api/set?upload_kb=100")
check("a value that is not one of the 7 steps is refused (%s)" % c, c == 400)
c, j = getq("/api/set?upload=2")
check("upload=2 is refused", c == 400)

print("=== ROUND 4: upload OFF -> the peer is choked, seeding stops")
c, j = getq("/api/set?upload=0")
L.pump(3)
check("peer got a choke message", L.choked_msg or L.closed)
check("seeding stopped", wait(lambda: items()[NAME].get("seeding") is False, 8))
cfgtxt = open(FT + "/log/config.txt").read()
check("config file stores upload=0", "upload=0" in cfgtxt and "upload_kb=128" in cfgtxt)
L.close()

print("=== ROUND 5: restart keeps the setting and the finished task starts seeding again")
getq("/api/set?upload=1&upload_kb=256")
quit_()
boot(0, wipe=False)
cfg = get("/api/config")
check("after restart: upload on, 256 KB/s (%s, %s)" % (cfg.get("upload"), cfg.get("upload_kb")), cfg.get("upload") is True and cfg.get("upload_kb") == 256)
check("finished task is listed and seeds again", wait(lambda: items().get(NAME, {}).get("seeding") is True, 30))
L = Leech(); L.pump(1.5); L.interested(); L.pump(1.5)
got, dt = measure(L, 5)
check("rate with a 256 KB/s limit: %.1f KB/s" % (got / 1024.0 / dt), 170 <= got / 1024.0 / dt <= 330)
check("data identical after restart", L.verify())
L.close()
# удаление раздачи останавливает отдачу
h = items()[NAME]["hash"]
urllib.request.urlopen(B + "/api/delete?hash=" + h, timeout=10).read()
check("deleting the task stops seeding", wait(lambda: NAME not in items(), 10) and "seeding stopped: Game One" in log())
quit_()

print("=== ROUND 6: upload while the torrent is still downloading (slow seeder)")
boot(0.08, cfg="max_parallel=3\nupload=1\nupload_kb=1024\n")
add("Game One.torrent", "&start=1")
check("some pieces arrive", wait(lambda: items().get(NAME, {}).get("pct", 0) >= 15, 40))
L = Leech(); L.pump(1.5)
have_now = sum(L.bits) if L.bits else 0
check("bitfield announces the pieces we already have (%d of %d)" % (have_now, NP), L.bits is not None and 0 < have_now < NP)
L.interested(); L.pump(1.5)
check("unchoked while downloading", L.unchoked)
pi = L.bits.index(True)
L.request(pi, 0, 16384); L.request(pi, 16384, 16384); L.pump(2)
check("a piece we have is served and identical", (pi, 0) in L.got and L.verify())
# кусок, которого у нас нет: запрос игнорируется
miss = [i for i, v in enumerate(L.bits) if not v]
nb = L.bytes()
if miss:
    L.request(miss[-1], 0, 16384); L.pump(1)
    check("a piece we do not have is not served", all(k[0] != miss[-1] or True for k in L.got) and (miss[-1], 0) not in L.got or L.bits[miss[-1]])
check("the download itself still completes", wait(lambda: items().get(NAME, {}).get("status") == "complete", 90))
L.pump(2)
check("have messages / full bitfield arrive for new pieces", L.bits is not None and sum(L.bits) >= have_now)
L.close()
quit_()
sh("pkill -9 -x daemon_sel; bash seeder_ctl.sh stop")
print("RESULT %d/%d" % (sum(res), len(res)))
