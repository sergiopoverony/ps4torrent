# Тестовый "входящий" сид: трекер пустой, сид сам подключается к демону на его порт.
import socket, threading, struct, hashlib, os, sys, time, http.server

sys.setrecursionlimit(10000)
SRC = "/tmp/ft/src"
NAME = os.environ.get("TORRENT", "Big8M")
DAEMON_PORT = int(os.environ.get("DAEMON_PORT", "26881"))
CONNECT_AFTER = float(os.environ.get("CONNECT_AFTER", "3"))
DELAY = float(os.environ.get("DELAY", "0"))

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

d = open("/tmp/ft/torrents_src/%s.torrent" % NAME, "rb").read()
top, _ = parse_bencode(d)
info_val, s0, e0 = top[b"info"]
IH = hashlib.sha1(d[s0:e0]).digest()
name = info_val[b"name"][0].decode()
PL = info_val[b"piece length"][0]
FILES = [(os.path.join(SRC, name, *[p.decode() for p in fe[b"path"][0]]), fe[b"length"][0]) for fe in info_val[b"files"][0]]
TOTAL = sum(l for _, l in FILES)
NP = (TOTAL + PL - 1) // PL

def read_range(off, n):
    out = b""; pos = 0
    for p, ln in FILES:
        if off < pos + ln and n > 0:
            with open(p, "rb") as f:
                f.seek(off - pos); chunk = f.read(min(n, pos + ln - off))
            out += chunk; off += len(chunk); n -= len(chunk)
        pos += ln
    return out

def recvn(c, n):
    b = b""
    while len(b) < n:
        x = c.recv(n - len(b))
        if not x: raise EOFError
        b += x
    return b

class T(http.server.BaseHTTPRequestHandler):       # пустой трекер
    def do_GET(self):
        body = b"d8:intervali60e5:peers0:e"
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass

def tracker():
    http.server.ThreadingHTTPServer(("127.0.0.1", 16969), T).serve_forever()

def inbound():
    time.sleep(CONNECT_AFTER)
    c = socket.create_connection(("127.0.0.1", DAEMON_PORT), timeout=10)
    c.sendall(b"\x13BitTorrent protocol" + b"\0"*8 + IH + b"-SEED02-abcdefghijkl")
    hs = recvn(c, 68)
    print("daemon handshake ok:", hs[28:48] == IH, flush=True)
    bf = bytearray((NP + 7)//8)
    for i in range(NP): bf[i//8] |= 0x80 >> (i % 8)
    c.sendall(struct.pack(">IB", 1 + len(bf), 5) + bytes(bf))
    c.sendall(struct.pack(">IB", 1, 1))
    served = 0
    c.settimeout(60)
    while True:
        ln = struct.unpack(">I", recvn(c, 4))[0]
        if ln == 0: continue
        msg = recvn(c, ln)
        if msg[0] == 2: c.sendall(struct.pack(">IB", 1, 1))
        elif msg[0] == 6:
            idx, begin, length = struct.unpack(">III", msg[1:13])
            if DELAY: time.sleep(DELAY)
            data = read_range(idx * PL + begin, length)
            c.sendall(struct.pack(">IBII", 9 + len(data), 7, idx, begin) + data)
            served += 1
            if served % 100 == 0: print("served", served, flush=True)

threading.Thread(target=tracker, daemon=True).start()
try:
    inbound()
except Exception as e:
    print("inbound ended:", repr(e), flush=True)
time.sleep(5)
