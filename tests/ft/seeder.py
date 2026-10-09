import socket, threading, struct, hashlib, os, sys, time, http.server, urllib.parse
SRC = "/tmp/ft/src"
DELAY = float(os.environ.get("DELAY", "0"))
BURST = float(os.environ.get("BURST", "0"))   # пачками: после каждых 40 блоков пауза BURST секунд
STALL_AFTER = int(os.environ.get("STALL_AFTER", "0"))
STALL_SECONDS = float(os.environ.get("STALL_SECONDS", "0"))   # если задано: молчим только столько секунд, потом снова отвечаем
stall_until = [0.0]   # после стольких блоков молчим (не отвечаем на запросы)   # секунд на каждый блок (чтобы замедлить)
torrents = {}   # info_hash -> (name, files(list of (path,len)), piece_length)

sys.path.insert(0, "/tmp/ft")
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
        i += 1; out = {}; 
        while d[i:i+1] != b"e":
            k, i = parse_bencode(d, i); v, i2 = parse_bencode(d, i)
            out[k] = (v, i, i2); i = i2
        return out, i+1
    colon = d.index(b":", i); n = int(d[i:colon]); s = d[colon+1:colon+1+n]
    return s, colon+1+n

def load(path):
    d = open(path, "rb").read()
    top, _ = parse_bencode(d)
    info_val, s, e = top[b"info"]
    ih = hashlib.sha1(d[s:e]).digest()
    info = info_val
    name = info[b"name"][0].decode()
    pl = info[b"piece length"][0]
    files = []
    for fe in info[b"files"][0]:
        files.append((os.path.join(SRC, name, *[p.decode() for p in fe[b"path"][0]]), fe[b"length"][0]))
    torrents[ih] = (name, files, pl)

for fn in os.listdir("/tmp/ft/torrents_src"): load("/tmp/ft/torrents_src/" + fn)

def read_range(files, off, n):
    out = b""
    pos = 0
    for p, ln in files:
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

def serve(c):
    bc = [0]
    try:
        hs = recvn(c, 68)
        ih = hs[28:48]
        if ih not in torrents: c.close(); return
        name, files, pl = torrents[ih]
        total = sum(l for _, l in files)
        npieces = (total + pl - 1) // pl
        c.sendall(b"\x13BitTorrent protocol" + b"\0"*8 + ih + b"-SEED01-123456789012")
        bf = bytearray((npieces + 7)//8)
        for i in range(npieces): bf[i//8] |= 0x80 >> (i % 8)
        c.sendall(struct.pack(">IB", 1 + len(bf), 5) + bytes(bf))
        c.sendall(struct.pack(">IB", 1, 1))   # unchoke
        while True:
            ln = struct.unpack(">I", recvn(c, 4))[0]
            if ln == 0: continue
            msg = recvn(c, ln)
            mid = msg[0]
            if mid == 2: c.sendall(struct.pack(">IB", 1, 1))
            elif mid == 6:
                served = globals().get("served", 0) + 1; globals()["served"] = served
                if STALL_AFTER and served == STALL_AFTER + 1 and STALL_SECONDS and not stall_until[0]:
                    stall_until[0] = time.time() + STALL_SECONDS
                if STALL_AFTER and served > STALL_AFTER and (not STALL_SECONDS or time.time() < stall_until[0]):
                    globals()["served"] = served - 1   # этот запрос не обслужен
                    continue   # молчим: запрос остаётся без ответа
                idx, begin, length = struct.unpack(">III", msg[1:13])
                if served % 25 == 0:
                    open("/tmp/ft/served.txt", "w").write(str(served))
                data = read_range(files, idx * pl + begin, length)
                if DELAY: time.sleep(DELAY)
                c.sendall(struct.pack(">IBII", 9 + len(data), 7, idx, begin) + data)
                bc[0] += 1
                if BURST and bc[0] % 40 == 0: time.sleep(BURST)
    except Exception as e:
        pass
    finally:
        try: c.close()
        except: pass

def listen():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 16881)); s.listen(20)
    while True:
        c, _ = s.accept(); threading.Thread(target=serve, args=(c,), daemon=True).start()

class T(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        time.sleep(float(os.environ.get("TRACKER_DELAY", "0")))   # медленный трекер
        peers = b"".join(socket.inet_aton("127.0.0.1") + struct.pack(">H", pt) for pt in (16881, 16882, 16883, 16999, 16998, 26881))
        body = b"d8:intervali60e5:peers" + str(len(peers)).encode() + b":" + peers + b"e"
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass

def bad_listen(port, mode):
    sk = socket.socket(); sk.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sk.bind(("127.0.0.1", port)); sk.listen(20)
    while True:
        c, _ = sk.accept()
        try:
            if mode == "garbage":
                c.recv(68); c.sendall(b"\x16\x03\x01" + os.urandom(65))   # как будто TLS/шифрование
            # mode == "closer": сразу закрываем
        except Exception: pass
        c.close()
threading.Thread(target=bad_listen, args=(16882, "garbage"), daemon=True).start()
threading.Thread(target=bad_listen, args=(16883, "closer"), daemon=True).start()
threading.Thread(target=listen, daemon=True).start()
http.server.ThreadingHTTPServer(("127.0.0.1", 16969), T).serve_forever()
