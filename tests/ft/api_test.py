import urllib.request, urllib.error, json, time, os, socket, sys
B = "http://127.0.0.1:18787"; TK = "sekret"; R = "/tmp/ft/usb0/torrents"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def req(path, data=None, method=None, token=True, timeout=8):
    url = B + path + ((("&" if "?" in path else "?") + "token=" + TK) if token else "")
    r = urllib.request.Request(url, data=data, method=method)
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp: return resp.status, resp.read()
    except urllib.error.HTTPError as e: return e.code, e.read()
def jreq(*a, **k):
    code, body = req(*a, **k)
    try: return code, json.loads(body)
    except Exception: return code, {}
def raw(data, wait=3, close_after_send=False):
    s = socket.create_connection(("127.0.0.1", 18787), timeout=wait + 2); s.sendall(data)
    if close_after_send: s.shutdown(socket.SHUT_WR)
    out = b""; s.settimeout(wait)
    try:
        while True:
            x = s.recv(4096)
            if not x: break
            out += x
    except Exception: pass
    s.close(); return out

small = open("/tmp/ft/torrents_src/Small.torrent", "rb").read()
big = open("/tmp/ft/torrents_src/Big8M.torrent", "rb").read()

print("== access token")
code, _ = req("/status", token=False); check("/status without token -> 401", code == 401)
code, _ = req("/api/pause?hash=" + "a"*40, token=False); check("/api/pause without token -> 401", code == 401)
code, _ = req("/quit", token=False); check("/quit without token -> 401", code == 401)
code, body = req("/", token=False); check("page '/' is served without token", code == 200 and b"ps4torrent" in body)
code, _ = req("/status?token=wrong", token=False); check("wrong token -> 401", code == 401)
code, d = jreq("/status"); check("/status with token -> 200", code == 200 and "items" in d)

print("== add .torrent")
code, d = jreq("/api/add?drive=0&name=" + urllib.parse.quote("Моя игра.torrent"), data=small, method="POST")
check("valid upload accepted, Cyrillic name kept: %s" % d.get("file"), code == 200 and d.get("ok") and d["file"] == "Моя игра.torrent" and os.path.exists(R + "/Моя игра.torrent"))
code, d = jreq("/api/add?drive=0&name=" + urllib.parse.quote("../../etc/passwd"), data=big, method="POST")
check("path traversal in name is neutralised: %s" % d.get("file"), code == 200 and d["file"] == "etcpasswd.torrent" and os.path.exists(R + "/etcpasswd.torrent") and not os.path.exists("/tmp/ft/etc"))
code, d = jreq("/api/add?drive=0&name=" + urllib.parse.quote('a:b*c?d"e<f>g|h.torrent'), data=small, method="POST")
check("forbidden characters removed: %s" % d.get("file"), code == 200 and d["file"] == "abcdefgh.torrent")
code, d = jreq("/api/add?drive=0&name=" + urllib.parse.quote("Моя игра.torrent"), data=small, method="POST")
check("existing name is not overwritten: %s" % d.get("file"), code == 200 and d["file"] == "Моя игра (2).torrent")
code, d = jreq("/api/add?drive=0&name=bad.torrent", data=os.urandom(500), method="POST")
check("garbage body rejected (400): %s" % d.get("error"), code == 400 and not d.get("ok") and not os.path.exists(R + "/bad.torrent"))
code, d = jreq("/api/add?drive=0&name=empty.torrent", data=b"", method="POST"); check("empty body rejected (400)", code == 400)
out = raw(b"POST /api/add?name=huge.torrent&token=sekret HTTP/1.0\r\nContent-Length: 5000000\r\n\r\n")
check("announced size > 1 MB rejected (413)", b" 413 " in out.split(b"\r\n")[0])
out = raw(b"POST /api/add?name=cut.torrent&token=sekret HTTP/1.0\r\nContent-Length: 1000\r\n\r\n" + b"d8:announce", close_after_send=True)
check("incomplete upload rejected (408)", b" 408 " in out.split(b"\r\n")[0])
code, d = jreq("/api/add?drive=0&name=x.torrent"); check("GET on /api/add -> 405", code == 405)
check("no temp file left behind", not [f for f in os.listdir(R) if f.startswith(".upload")])
time.sleep(3)
code, d = jreq("/status"); titles = [i["title"] for i in d["items"]]
check("uploaded torrents were picked up immediately: %s" % titles[:5], any("Big8M" in t or "etcpasswd" in t for t in titles))

print("== robustness")
out = raw(b"POST /api/add?token=sekret HTTP/1.0\r\nX-Pad: " + b"a" * 20000 + b"\r\n\r\n"); check("POST with oversized headers rejected (431)", b" 431 " in out.split(b"\r\n")[0])
out = raw(b"GET /status?token=sekret HTTP/1.0\r\nX-Pad: " + b"a" * 20000 + b"\r\n\r\n"); check("GET with a huge header is still answered normally (rest ignored)", b" 200 " in out.split(b"\r\n")[0])
out = raw(b"PUT /status HTTP/1.0\r\n\r\n"); check("PUT -> 405", b" 405 " in out.split(b"\r\n")[0])
out = raw(b"GET /nope?token=sekret HTTP/1.0\r\n\r\n"); check("unknown path -> 404", b" 404 " in out.split(b"\r\n")[0])

print("== settings")
code, d = jreq("/api/config"); check("config shows token_set and defaults: %s" % d, d.get("token_set") is True and d.get("max_parallel") == 3 and d.get("listen_port") == 26881)
code, d = jreq("/api/set?max_parallel=26"); check("max_parallel=26 rejected (400)", code == 400)
code, d = jreq("/api/set?max_parallel=0"); check("max_parallel=0 rejected (400)", code == 400)
code, d = jreq("/api/set?max_parallel=25"); check("max_parallel=25 accepted", code == 200 and d.get("ok"))
code, d = jreq("/api/set?listen_port=80"); check("listen_port=80 rejected (400)", code == 400)
code, d = jreq("/api/set"); check("empty set rejected (400)", code == 400)
code, d = jreq("/api/set?max_parallel=2&listen_port=26999"); check("valid set accepted", code == 200 and d.get("ok"))
time.sleep(2)
code, d = jreq("/api/config"); check("config reflects new values: %s" % d, d.get("max_parallel") == 2 and d.get("listen_port") == 26999)
def can_connect(p):
    try: socket.create_connection(("127.0.0.1", p), timeout=2).close(); return True
    except Exception: return False
check("new port 26999 accepts connections", can_connect(26999)); check("old port 26881 is closed now", not can_connect(26881))
cfg = open("/tmp/ft/log/config.txt").read(); print("config.txt:", cfg.replace("\n", " | "))
check("config.txt persisted (and keeps the token)", "max_parallel=2" in cfg and "listen_port=26999" in cfg and "web_token=sekret" in cfg)
print("RESULT: %d/%d passed" % (sum(res), len(res)))
