# Загрузки по прямой ссылке http:// (бинарник daemon_direct: повтор после обрыва через 2 с).
# Тестовый веб-сервер (в этом же процессе) отдаёт файлы с докачкой и без неё, с обрывами, переадресациями, .torrent.
import os, sys, time, json, base64, hashlib, threading, subprocess, urllib.request, urllib.error, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; BIN = sys.argv[1] if len(sys.argv) > 1 else "daemon_direct"
SRV = 18900; S = "http://127.0.0.1:%d" % SRV
DL = FT + "/usb0/torrents/downloads"
res = []
def check(name, ok): ok = bool(ok); res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def getq(p, t=45):
    try: return 200, json.load(urllib.request.urlopen(B + p, timeout=t))
    except urllib.error.HTTPError as e: return e.code, json.loads(e.read())
def q(u): return urllib.parse.quote(u, safe="")
def items(): return {i["title"]: i for i in getq("/status")[1]["items"]}
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.3)
    return False
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def md5(p): return hashlib.md5(open(p, "rb").read()).hexdigest()

# ---- данные
os.makedirs(FT + "/httpsrc", exist_ok=True)
BIG = FT + "/httpsrc/big.bin"
if not os.path.exists(BIG) or os.path.getsize(BIG) != 12_000_000:
    open(BIG, "wb").write(os.urandom(12_000_000))
BIGDATA = open(BIG, "rb").read(); BIGMD5 = hashlib.md5(BIGDATA).hexdigest()
TOR = open(FT + "/torrents_src/Game One.torrent", "rb").read()

RATE = [3_000_000]                 # байт/с (общая скорость отдачи файла)
ranges = []                        # (путь, начало Range)
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, *a): pass
    def send_body(self, data, start=0, total=None, code=200, drop_after=None, ctype="application/octet-stream", extra=None, nolen=False):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        if not nolen: self.send_header("Content-Length", str(len(data) - start))
        if code == 206: self.send_header("Content-Range", "bytes %d-%d/%d" % (start, len(data) - 1, len(data)))
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers()
        pos, sent, chunk = start, 0, 32768
        try:
            while pos < len(data):
                n = min(chunk, len(data) - pos)
                if drop_after is not None and sent + n > drop_after: n = drop_after - sent
                if n <= 0: return
                self.wfile.write(data[pos:pos + n]); pos += n; sent += n
                time.sleep(n / RATE[0])
        except Exception: pass
    def do_GET(self):
        p = self.path
        rng = self.headers.get("Range")
        start = int(rng.split("=")[1].split("-")[0]) if rng else 0
        ranges.append((p, start))
        if p in ("/files/big.bin", "/files/big2.bin", "/a/big.bin", "/b/big.bin"):
            return self.send_body(BIGDATA, start, code=206 if rng else 200)
        if p == "/norange/big.bin": return self.send_body(BIGDATA)
        if p == "/drop/big.bin": return self.send_body(BIGDATA, start, code=206 if rng else 200, drop_after=3_000_000)
        if p == "/nosize/big.bin": return self.send_body(BIGDATA[:2_000_000], nolen=True)
        if p == "/redir":
            self.send_response(302); self.send_header("Location", "/files/big.bin"); self.end_headers(); return
        if p == "/redir_https":
            self.send_response(301); self.send_header("Location", "https://example.com/x.bin"); self.end_headers(); return
        if p == "/t/game.torrent": return self.send_body(TOR, ctype="application/x-bittorrent")
        if p == "/t/download?id=5": return self.send_body(TOR)
        if p == "/cd":
            return self.send_body(BIGDATA[:100000], extra={"Content-Disposition": "attachment; filename*=UTF-8''%D0%98%D0%B3%D1%80%D0%B0%201.pkg"})
        self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers()
srv = ThreadingHTTPServer(("127.0.0.1", SRV), H); srv.daemon_threads = True
threading.Thread(target=srv.serve_forever, daemon=True).start()

def boot(wipe=True):
    if wipe: sh("pkill -9 -x %s; rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log" % BIN)
    else: sh("pkill -9 -x %s" % BIN)
    subprocess.Popen("setsid ./%s >> direct.out 2>&1 < /dev/null &" % BIN, shell=True, cwd=FT); time.sleep(2.5)
def quit_():
    try: urllib.request.urlopen(B + "/quit", timeout=3).read()
    except Exception: pass
    wait(lambda: subprocess.run("pgrep -x %s" % BIN, shell=True, stdout=subprocess.DEVNULL).returncode != 0, 15)
def add_url(url, name, size, known=1, start=1, drive="0"):
    return getq("/api/add_url?url=%s&name=%s&size=%d&size_known=%d&drive=%s&start=%d" % (q(url), q(name), size, known, drive, start))

open(FT + "/direct.out", "w").close()
boot()
print("=== probe", flush=True)
c, d = getq("/api/url_probe?url=" + q(S + "/t/game.torrent"))
check("probe .torrent by name: torrent=true, same bytes", c == 200 and d["torrent"] and base64.b64decode(d["data"]) == TOR and d["name"] == "game.torrent")
c, d = getq("/api/url_probe?url=" + q(S + "/t/download?id=5"))
check("probe .torrent without .torrent in the address (by content)", c == 200 and d["torrent"] and base64.b64decode(d["data"]) == TOR and d["name"].endswith(".torrent"))
c, d = getq("/api/url_probe?url=" + q(S + "/files/big.bin"))
check("probe a plain file: name, size, not a torrent", c == 200 and not d["torrent"] and d["name"] == "big.bin" and d["size"] == 12_000_000 and d["size_known"])
c, d = getq("/api/url_probe?url=" + q(S + "/redir"))
check("probe follows a redirect", c == 200 and d["name"] == "big.bin" and d["size"] == 12_000_000)
c, d = getq("/api/url_probe?url=" + q("https://example.com/a.pkg"))
check("https:// link is refused with a clear message", c == 400 and "https" in d["error"])
c, d = getq("/api/url_probe?url=" + q(S + "/redir_https"))
check("redirect to https:// is reported", c == 502 and "https" in d["error"])
c, d = getq("/api/url_probe?url=" + q(S + "/missing.bin"))
check("404 is reported", c == 502 and "404" in d["error"])
c, d = getq("/api/url_probe?url=" + q(S + "/cd"))
check("name from Content-Disposition (UTF-8)", c == 200 and d["name"] == "Игра 1.pkg")
c, d = getq("/api/url_probe?url=" + q("ftp://x/y"))
check("non-http link refused", c == 400)

print("=== download, duplicate, pause/resume", flush=True)
c, d = add_url(S + "/files/big.bin", "big.bin", 12_000_000)
check("add_url accepted", c == 200 and d["ok"] and d["started"])
check("item appears as kind=http and downloads", wait(lambda: items()["big.bin"]["kind"] == "http" and items()["big.bin"]["status"] == "downloading" and items()["big.bin"]["done"] > 1_000_000, 15))
c, d = add_url(S + "/files/big.bin", "big.bin", 12_000_000)
check("the same link again -> duplicate", c == 200 and d.get("duplicate"))
c, d = getq("/api/url_probe?url=" + q(S + "/files/big.bin"))
check("probe shows duplicate while in the list", c == 200 and d["duplicate"])
h = items()["big.bin"]["hash"]
check("hash is 40 hex characters", len(h) == 40)
getq("/api/pause?hash=" + h)
check("pause -> paused", wait(lambda: items()["big.bin"]["status"] == "paused", 5))
d1 = items()["big.bin"]["done"]; time.sleep(2); d2 = items()["big.bin"]["done"]
check("no progress while paused", d1 == d2 and 0 < d1 < 12_000_000)
check(".part file on the drive while not finished", os.path.exists(DL + "/big.bin.part"))
n0 = len(ranges)
getq("/api/resume?hash=" + h)
check("resume continues with a Range request from the saved size", wait(lambda: any(p == "/files/big.bin" and s > 0 for p, s in ranges[n0:]), 8))
check("finishes: complete", wait(lambda: items()["big.bin"]["status"] == "complete", 30))
check("file is identical, .part is gone", os.path.exists(DL + "/big.bin") and md5(DL + "/big.bin") == BIGMD5 and not os.path.exists(DL + "/big.bin.part"))
it = items()["big.bin"]
check("complete item: size and done = 12000000, pct 100", it["size"] == 12_000_000 and it["done"] == 12_000_000 and it["pct"] == 100)

print("=== name collision, restart in the middle", flush=True)
RATE[0] = 1_500_000
c, d = add_url(S + "/a/big.bin", "big.bin", 12_000_000)
check("second file with the same name gets a new name", wait(lambda: "big (2).bin" in items(), 5))
check("it downloads", wait(lambda: items()["big (2).bin"]["done"] > 2_000_000, 15))
quit_()
check("after quit: .part kept", os.path.exists(DL + "/big (2).bin.part"))
n0 = len(ranges)
boot(wipe=False)
check("after restart: still in the list", "big (2).bin" in items() and items()["big.bin"]["status"] == "complete")
check("after restart: resumes with Range", wait(lambda: any(p == "/a/big.bin" and s > 0 for p, s in ranges[n0:]), 10))
RATE[0] = 6_000_000
check("after restart: completes, identical", wait(lambda: items()["big (2).bin"]["status"] == "complete", 30) and md5(DL + "/big (2).bin") == BIGMD5)

print("=== server without resume, connection drops, unknown size", flush=True)
RATE[0] = 2_000_000
add_url(S + "/norange/big.bin", "nr.bin", 12_000_000)
check("no-range download starts", wait(lambda: items()["nr.bin"]["done"] > 2_000_000, 15))
h = items()["nr.bin"]["hash"]
getq("/api/pause?hash=" + h); wait(lambda: items()["nr.bin"]["status"] == "paused", 5)
RATE[0] = 8_000_000
getq("/api/resume?hash=" + h)
check("no-range: starts over and completes identical", wait(lambda: items()["nr.bin"]["status"] == "complete", 30) and md5(DL + "/nr.bin") == BIGMD5)
add_url(S + "/drop/big.bin", "drop.bin", 12_000_000)
check("dropping server: retries and completes identical", wait(lambda: items()["drop.bin"]["status"] == "complete", 60) and md5(DL + "/drop.bin") == BIGMD5)
check("dropping server: the drop was seen as waiting/retry in the log", "retry in" in open(FT + "/log/log.txt").read())
add_url(S + "/nosize/big.bin", "ns.bin", 0, known=0)
check("unknown size: completes, size learned", wait(lambda: items()["ns.bin"]["status"] == "complete", 20) and items()["ns.bin"]["size"] == 2_000_000 and os.path.getsize(DL + "/ns.bin") == 2_000_000)

print("=== errors, delete, pause all, space", flush=True)
add_url(S + "/missing.bin", "missing.bin", 0, known=0)
check("404 -> error status with text", wait(lambda: items()["missing.bin"]["status"] == "error" and "404" in items()["missing.bin"]["error"], 10))
n0 = len([r for r in ranges if r[0] == "/missing.bin"]); time.sleep(5)
check("404 is not retried by itself", len([r for r in ranges if r[0] == "/missing.bin"]) == n0)
h = items()["missing.bin"]["hash"]
getq("/api/delete?hash=" + h)
check("delete removes the item", wait(lambda: "missing.bin" not in items(), 5))
h = items()["ns.bin"]["hash"]
getq("/api/delete?hash=" + h)
check("delete (without files) keeps the file", wait(lambda: "ns.bin" not in items(), 5) and os.path.exists(DL + "/ns.bin"))
h = items()["nr.bin"]["hash"]
getq("/api/delete?files=1&hash=" + h)
check("delete with files removes the file", wait(lambda: "nr.bin" not in items(), 5) and not os.path.exists(DL + "/nr.bin"))
RATE[0] = 1_000_000
add_url(S + "/b/big.bin", "pa.bin", 12_000_000)
wait(lambda: items()["pa.bin"]["status"] == "downloading", 10)
getq("/api/pause_all")
check("pause all pauses the link download", wait(lambda: items()["pa.bin"]["status"] == "paused", 5))
getq("/api/resume_all")
check("resume all resumes it", wait(lambda: items()["pa.bin"]["status"] == "downloading", 8))
h = items()["pa.bin"]["hash"]
getq("/api/delete?files=1&hash=" + h)
check("delete while downloading: item and .part removed", wait(lambda: "pa.bin" not in items() and not os.path.exists(DL + "/pa.bin.part"), 8))
c, d = add_url(S + "/files/big2.bin", "huge.bin", 10**15)
check("not enough space -> 507", c == 507 and "space" in d["error"])
c, d = add_url(S + "/files/big2.bin", "", 1)
check("empty name -> 400", c == 400)
st = getq("/status")[1]
check("status JSON still valid with torrents list empty and link items", isinstance(st["items"], list) and all("hash" in i for i in st["items"]))
quit_()
out = open(FT + "/direct.out").read()
check("no sanitizer errors", "ERROR: AddressSanitizer" not in out and "runtime error" not in out)
srv.shutdown()
print("RESULT %d/%d" % (sum(res), len(res)))
