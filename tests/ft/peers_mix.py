# Смесь пиров для проверки быстрой замены: хорошие (медленно отдают), молчащие (всё обещают и молчат), «закрытые» (никогда не разрешают качать).
# Запуск: python3 peers_mix.py  (трекер на 16969, пиры 17001-17003 хорошие, 17011-17016 молчащие, 17021-17026 закрытые)
import os, sys, socket, struct, threading, time, json, http.server
os.environ.setdefault("DELAY", "0.1")
src = open("/tmp/ft/seeder.py").read().split("def listen")[0]
exec(src)
GOOD = [17001, 17002, 17003]; SILENT = list(range(17011, 17017)); CHOKED = list(range(17021, 17027))
stats = {}
def dead(c, port, mode):
    t0 = time.time(); stats[port] = {"mode": mode, "t0": t0, "closed": None}
    try:
        hs = recvn(c, 68); ih = hs[28:48]
        if ih not in torrents: return
        name, files, pl = torrents[ih]
        total = sum(l for _, l in files); npieces = (total + pl - 1) // pl
        c.sendall(b"\x13BitTorrent protocol" + b"\0"*8 + ih + b"-DEAD01-12345678901"+ bytes([48 + port % 10]))
        bf = bytearray((npieces + 7)//8)
        for i in range(npieces): bf[i//8] |= 0x80 >> (i % 8)
        c.sendall(struct.pack(">IB", 1 + len(bf), 5) + bytes(bf))
        if mode == "silent": c.sendall(struct.pack(">IB", 1, 1))       # unchoke, но на запросы не отвечаем
        while True:
            x = c.recv(4096)
            if not x: break
    except Exception: pass
    finally:
        stats[port]["closed"] = time.time()
        try: c.close()
        except Exception: pass
def listener(port, fn):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", port)); s.listen(20)
    while True:
        c, _ = s.accept(); threading.Thread(target=fn, args=(c,), daemon=True).start()
for p in GOOD: threading.Thread(target=listener, args=(p, serve), daemon=True).start()
for p in SILENT: threading.Thread(target=listener, args=(p, lambda c, p=p: dead(c, p, "silent")), daemon=True).start()
for p in CHOKED: threading.Thread(target=listener, args=(p, lambda c, p=p: dead(c, p, "choked")), daemon=True).start()
class T(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        ports = CHOKED + SILENT + GOOD            # хорошие в конце: сначала пробуются плохие (кандидаты берутся с конца списка)
        peers = b"".join(socket.inet_aton("127.0.0.1") + struct.pack(">H", pt) for pt in reversed(ports))
        body = b"d8:intervali60e5:peers" + str(len(peers)).encode() + b":" + peers + b"e"
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
threading.Thread(target=http.server.HTTPServer(("127.0.0.1", 16969), T).serve_forever, daemon=True).start()
while True:
    time.sleep(1)
    json.dump({str(k): v for k, v in stats.items()}, open("/tmp/ft/mix_stats.json", "w"))
