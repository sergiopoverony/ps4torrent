import socket, os, time, hashlib, sys, urllib.request, json
sys.setrecursionlimit(10000)
PORT = 26881
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
d = open("/tmp/ft/torrents_src/Big8M.torrent","rb").read()
top,_ = parse_bencode(d); _, a, b = top[b"info"]; IH = hashlib.sha1(d[a:b]).digest()

def hs(ih, pid=b"-NEG001-abcdefghijkl"): return b"\x13BitTorrent protocol" + b"\0"*8 + ih + pid
def conn(): return socket.create_connection(("127.0.0.1", PORT), timeout=5)
def closed_within(c, secs):
    """True, если соединение закрыто пиром (recv вернул b'') в течение secs секунд."""
    c.settimeout(secs); t=time.time()
    try:
        while time.time()-t < secs:
            x = c.recv(4096)
            if x == b"": return True
    except socket.timeout: return False
    except ConnectionResetError: return True
    return False
def status_ok():
    try:
        t=time.time(); d = json.load(urllib.request.urlopen("http://127.0.0.1:18787/status", timeout=3)); return time.time()-t < 2.5
    except Exception as e: return False
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)

# 1 чужой info_hash
c = conn(); c.sendall(hs(os.urandom(20))); check("wrong info_hash is closed", closed_within(c, 3)); c.close()
# 2 мусор
c = conn(); c.sendall(os.urandom(100)); check("random garbage is closed", closed_within(c, 3)); c.close()
# 3 HTTP вместо BitTorrent
c = conn(); c.sendall(b"GET / HTTP/1.0\r\n\r\n"); check("HTTP request is closed", closed_within(c, 3)); c.close()
# 4 дубликат: два соединения с одним peer id
c1 = conn(); c1.sendall(hs(IH, b"-DUP001-abcdefghijkl")); r1 = c1.recv(68)
time.sleep(0.5)
c2 = conn(); c2.sendall(hs(IH, b"-DUP001-abcdefghijkl")); 
dup_closed = closed_within(c2, 3)
check("first valid peer gets our handshake", len(r1) == 68 and r1[28:48] == IH)
check("duplicate peer id is closed", dup_closed)
c2.close()
# 5 молчаливое соединение: закроется по таймауту ~10 с
silent = conn()
# 6 обрезанный handshake (30 байт) и тишина
part = conn(); part.sendall(hs(IH)[:30])
# 7 наплыв: 60 молчаливых соединений
flood = []
for i in range(60):
    try: flood.append(conn())
    except Exception as e: pass
time.sleep(1.0)
check("daemon HTTP still responsive during flood", status_ok())
immediately_closed = sum(1 for f in flood if closed_within(f, 0.05))
print("  flood: %d of %d closed right away (cap on waiting connections)" % (immediately_closed, len(flood)), flush=True)
check("flood is capped (some closed at once)", immediately_closed >= 20)
time.sleep(11)
check("silent connection closed by timeout", closed_within(silent, 1))
check("partial handshake closed by timeout", closed_within(part, 1))
alive = sum(1 for f in flood if not closed_within(f, 0.05))
check("all flood connections gone after timeout", alive == 0)
check("daemon HTTP responsive after tests", status_ok())
c1.close()
print("RESULT: %d/%d passed" % (sum(res), len(res)))
