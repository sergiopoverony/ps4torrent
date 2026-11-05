#!/usr/bin/env python3
"""Magnet: запрос через /api/add_magnet -> .magnet в папке -> трекер -> пир отдаёт описание (BEP 9) -> готовый .torrent.
Поддельные трекер (порт 18950) и пир (18951) живут внутри теста. Бинарник: daemon_host. Запуск: magnet_run.sh"""
import base64, hashlib, json, os, socket, struct, threading, time, urllib.parse, urllib.request, urllib.error
from http.server import BaseHTTPRequestHandler, HTTPServer
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; I = FT + "/internal/torrents"
TRK, PEER = 18950, 18951
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def get(p):
    try: return json.load(urllib.request.urlopen(B + p, timeout=15))
    except urllib.error.HTTPError as e: return dict(json.loads(e.read()), _http=e.code)
def status(): return json.load(urllib.request.urlopen(B + "/status", timeout=8))
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.3)
    return False
def log(): return open(FT + "/log/log.txt").read()

# --- маленький bencode
def benc(x):
    if isinstance(x, int): return b"i%de" % x
    if isinstance(x, bytes): return b"%d:%s" % (len(x), x)
    if isinstance(x, str): return benc(x.encode())
    if isinstance(x, list): return b"l" + b"".join(benc(i) for i in x) + b"e"
    if isinstance(x, dict): return b"d" + b"".join(benc(k) + benc(x[k]) for k in sorted(x)) + b"e"
def bdec(d, p=0):
    c = d[p:p+1]
    if c == b"i":
        e = d.index(b"e", p); return int(d[p+1:e]), e + 1
    if c == b"l":
        p += 1; out = []
        while d[p:p+1] != b"e":
            v, p = bdec(d, p); out.append(v)
        return out, p + 1
    if c == b"d":
        p += 1; out = {}; 
        while d[p:p+1] != b"e":
            k, p = bdec(d, p); v, p = bdec(d, p); out[k] = v
        return out, p + 1
    e = d.index(b":", p); n = int(d[p:e]); return d[e+1:e+1+n], e + 1 + n
def raw_info(path):
    """байты словаря info как есть: ищем границы через разбор"""
    d = open(path, "rb").read()
    p = 1
    while True:
        k, p = bdec(d, p)
        s = p; v, p = bdec(d, p)
        if k == b"info": return d[s:p]

# --- данные для раздачи
INFO = {}   # info hash (bytes) -> (info bytes, corrupt)
def reg(name, corrupt=False):
    info = raw_info(FT + "/torrents_src/" + name)
    h = hashlib.sha1(info).digest()
    INFO[h] = (info, corrupt)
    return h
def magnet(h, tr=True, b32=False, name=""):
    xt = base64.b32encode(h).decode() if b32 else h.hex().upper()
    m = "magnet:?xt=urn:btih:" + xt
    if tr: m += "&tr=" + urllib.parse.quote("http://127.0.0.1:%d/ann" % TRK, safe="")
    if name: m += "&dn=" + urllib.parse.quote(name)
    return m
def add(m, extra=""): return get("/api/add_magnet?magnet=" + urllib.parse.quote(m, safe="") + extra)

# --- поддельный трекер
announces = []
class T(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        announces.append(self.path)
        body = benc({"interval": 1800, "peers": socket.inet_aton("127.0.0.1") + struct.pack(">H", PEER)})
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
class Srv(HTTPServer): allow_reuse_address = True
trk = Srv(("127.0.0.1", TRK), T); threading.Thread(target=trk.serve_forever, daemon=True).start()

# --- поддельный пир
served = []
def recv_n(c, n):
    b = b""
    while len(b) < n:
        x = c.recv(n - len(b))
        if not x: raise EOFError
        b += x
    return b
def peer_conn(c):
    try:
        c.settimeout(10)
        hs = recv_n(c, 68); ih = hs[28:48]
        if ih not in INFO: return
        info, corrupt = INFO[ih]
        data = bytearray(info)
        if corrupt: data[len(data) // 2] ^= 0xFF
        c.sendall(b"\x13BitTorrent protocol" + bytes([0, 0, 0, 0, 0, 0x10, 0, 0]) + ih + b"-PY0001-123456789012")
        hsx = benc({"m": {"ut_metadata": 3}, "metadata_size": len(info)})
        c.sendall(struct.pack(">IBB", len(hsx) + 2, 20, 0) + hsx)
        while True:
            ln = struct.unpack(">I", recv_n(c, 4))[0]
            if ln == 0: continue
            m = recv_n(c, ln)
            if m[0] != 20 or m[1] != 3: continue
            req, _ = bdec(m[2:])
            if req[b"msg_type"] != 0: continue
            i = req[b"piece"]; served.append(i)
            chunk = bytes(data[i*16384:(i+1)*16384])
            hd = benc({"msg_type": 1, "piece": i, "total_size": len(info)})
            pl = hd + chunk
            c.sendall(struct.pack(">IBB", len(pl) + 2, 20, 1) + pl)
    except Exception: pass
    finally: c.close()
ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); ls.bind(("127.0.0.1", PEER)); ls.listen(20)
def acceptor():
    while True:
        c, _ = ls.accept(); threading.Thread(target=peer_conn, args=(c,), daemon=True).start()
threading.Thread(target=acceptor, daemon=True).start()

def titles(): return {i["title"]: i for i in status()["items"]}

print("== bad links")
r = get("/api/add_magnet?magnet=" + urllib.parse.quote("http://x/y", safe=""))
check("not a magnet -> 400: %s" % r, r.get("_http") == 400)
r = get("/api/add_magnet?magnet=" + urllib.parse.quote("magnet:?xt=urn:btih:ZZZ", safe=""))
check("bad hash -> 400: %s" % r, r.get("_http") == 400)
r = get("/api/add_magnet")
check("no parameter -> 400", r.get("_http") == 400)

print("== normal request (hex hash, tracker in the link)")
h1 = reg("Small.torrent")
r = add(magnet(h1, name="Small test"))
check("reply ok, pending, hash echoed: %s" % r, r.get("ok") and r.get("pending") and r.get("hash") == h1.hex())
check("the .magnet file is in the console memory folder", os.path.exists("%s/%s.magnet" % (I, h1.hex())) or os.path.exists("%s/Small.torrent" % I))
check("a .torrent appears and the .magnet disappears", wait(lambda: os.path.exists(I + "/Small.torrent") and not os.path.exists("%s/%s.magnet" % (I, h1.hex())), 25))
got = open(I + "/Small.torrent", "rb").read() if os.path.exists(I + "/Small.torrent") else b""
check("the saved .torrent has our announce URL and the same info hash", b"/ann" in got and hashlib.sha1(raw_info(I + "/Small.torrent")).digest() == h1)
check("the info bytes are identical to the original", got and raw_info(I + "/Small.torrent") == INFO[h1][0])
check("the tracker was asked with the right info_hash", any(urllib.parse.quote_from_bytes(h1) in a for a in announces))
check("the task shows up in the list", wait(lambda: "Small" in titles(), 20))
check("/api/magnets is empty again", get("/api/magnets")["magnets"] == [])

print("== duplicate")
r = add(magnet(h1))
check("already in the list -> duplicate: %s" % r, r.get("ok") and r.get("duplicate") is True)
check("no new .magnet file", not os.path.exists("%s/%s.magnet" % (I, h1.hex())))

print("== base32 hash, no tracker in the link (the built-in default is used, which is unreachable here -> stays pending)")
h2 = reg("Many.torrent")
r = add(magnet(h2, tr=False, b32=True))
check("accepted: %s" % r, r.get("ok") and r.get("hash") == h2.hex())
ok = wait(lambda: get("/api/magnets")["magnets"] and get("/api/magnets")["magnets"][0]["fails"] >= 1, 60)
m = get("/api/magnets")["magnets"]
check("stays pending with an error and a failure count: %s" % m, ok and m[0]["hash"] == h2.hex() and m[0]["error"] != "")
check(".magnet file is still there", os.path.exists("%s/%s.magnet" % (I, h2.hex())))
# тот же запрос, но со своим трекером в ссылке: файл уже есть, его содержимое не меняется (ждём очередной попытки)
check("log has the failure reason", "failed (" in log())

print("== a peer that sends corrupted metadata")
h3 = reg("GameOnePart.torrent", corrupt=True)
r = add(magnet(h3))
ok = wait(lambda: any(x["hash"] == h3.hex() and x["fails"] >= 1 for x in get("/api/magnets")["magnets"]), 40)
m = [x for x in get("/api/magnets")["magnets"] if x["hash"] == h3.hex()]
check("rejected by SHA-1, stays pending: %s" % m, ok and "none gave the metadata" in m[0]["error"])
check("no .torrent was created from bad data", not os.path.exists(I + "/GameOnePart.torrent"))

print("== a bad .magnet file is set aside")
open(I + "/broken.magnet", "w").write("hello\n")
check("renamed to .bad", wait(lambda: os.path.exists(I + "/broken.magnet.bad") and not os.path.exists(I + "/broken.magnet"), 15))

print("== a file with several links (any name), one per line, and the page button")
h4 = reg("Big8M.torrent"); h5 = reg("Game One.torrent")
open(I + "/games.magnet", "w").write("# my list\n" + magnet(h4, name="Big").replace("magnet:?", "") + "\n\n" + h5.hex().upper() + "\nnot a link\n")
check("the line with a tracker (written without the leading magnet:?) becomes a torrent", wait(lambda: os.path.exists(I + "/Big8M.torrent"), 40))
check("the bare-hash line is queued as its own request (no tracker here: the default one is unreachable, so it waits)", wait(lambda: os.path.exists("%s/%s.magnet" % (I, h5.hex())), 10))
check("the list file is gone", not os.path.exists(I + "/games.magnet") and not os.path.exists("%s/%s.magnet" % (I, h4.hex())))
check("log reports 2 requests taken, 1 bad line", "games.magnet: 2 request(s) taken, 1 bad line(s)" in log())
page = urllib.request.urlopen(B + "/", timeout=5).read().decode()
check("the page has the + magnet button", 'id="magb"' in page and "/api/add_magnet" in page)

print("== quit is quick even with magnets pending")
t0 = time.time(); urllib.request.urlopen(B + "/quit", timeout=5).read()
gone = wait(lambda: os.system("pgrep -x daemon_host >/dev/null") != 0, 20)
check("the daemon stopped in %.1f s" % (time.time() - t0), gone)

print("%d/%d" % (sum(res), len(res)))
