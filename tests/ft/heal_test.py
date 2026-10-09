import urllib.request, json, time, os, socket, subprocess, signal
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def status(t=5): return json.load(urllib.request.urlopen(B + "/status", timeout=t))
def page(t=5): return urllib.request.urlopen(B + "/", timeout=t).read()
def log(): return open(FT + "/log/log.txt").read()
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.2)
    return False
def can_connect(p):
    try: socket.create_connection(("127.0.0.1", p), timeout=2).close(); return True
    except Exception: return False
def get_retry(fn, secs=10):
    t0 = time.time()
    while time.time() - t0 < secs:
        try: return fn()
        except Exception: time.sleep(0.3)
    return None

print("== A. the listening socket breaks (accept keeps failing, like after console sleep)")
n0 = status()["net_heals"]
open(FT + "/accept_fail", "w").write("6")
for _ in range(4): get_retry(lambda: status(3), 3)           # обращения упираются в "сломанный" accept
ok = wait(lambda: status()["net_heals"] > n0, 12)
check("the HTTP thread noticed it and recreated the socket on its own (net_heals %d -> %s)" % (n0, get_retry(lambda: status()["net_heals"])), ok)
check("log says so", "the listening socket was broken (accept keeps failing, errno 163)" in log())
check("the web server works again", get_retry(lambda: status()["version"]) is not None)
check("the page itself loads", b"ps4torrent" in (get_retry(page) or b""))
if os.path.exists(FT + "/accept_fail"): os.remove(FT + "/accept_fail")

print("== B. the main loop freezes for 6 s (console sleep / stuck drive), then continues")
r0 = status()["net_restarts"]
open(FT + "/stall_main", "w").write("6")
time.sleep(1.5)
t = time.time(); body = page(); dt = time.time() - t
check("while the main loop is frozen the PAGE still loads (%.2f s): the web threads do not depend on it" % dt, b"ps4torrent" in body and dt < 2)
check("after the pause the listeners are refreshed (net_restarts %d -> %s)" % (r0, get_retry(lambda: status()["net_restarts"], 12)), wait(lambda: status()["net_restarts"] > r0, 12))
check("log: 'pause: the program was not running for N s'", wait(lambda: "pause: the program was not running for" in log(), 5))
check("web server answers after the pause", get_retry(lambda: status()["version"]) is not None)

print("== C. periodic quiet refresh (every 8 s in this test build)")
r1 = status()["net_restarts"]; time.sleep(20); r2 = status()["net_restarts"]
check("sockets were refreshed several times by themselves (%d -> %d)" % (r1, r2), r2 - r1 >= 2)

print("== D. the incoming-peers listener breaks")
open(FT + "/incoming_accept_fail", "w").write("5")
for _ in range(6):
    can_connect(26881); time.sleep(0.4)
check("log: accept keeps failing, then a new listener", wait(lambda: "incoming: accept keeps failing" in log() and "incoming: listener was broken, created a new one" in log(), 12))
check("port 26881 accepts connections again", wait(lambda: can_connect(26881), 8))
if os.path.exists(FT + "/incoming_accept_fail"): os.remove(FT + "/incoming_accept_fail")
print("RESULT: %d/%d passed" % (sum(res), len(res)))
