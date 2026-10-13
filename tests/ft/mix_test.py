# Быстрая замена бесполезных пиров. Аргумент: бинарник демона (daemon_host).
import urllib.request, json, time, os, subprocess, sys
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; BIN = sys.argv[1]
res = []
def check(name, ok): ok = bool(ok); res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def get(p, t=20): return urllib.request.urlopen(B + p, timeout=t).read()
def items(): return {i["title"]: i for i in json.loads(get("/status"))["items"]}
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
sh("pkill -9 -x %s; pkill -f '[p]eers_mix'; bash seeder_ctl.sh stop; rm -rf usb0 usb1 internal log mix_stats.json; mkdir -p usb0/torrents log" % BIN)
subprocess.Popen("setsid python3 peers_mix.py > mix.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2)
subprocess.Popen("setsid ./%s > mix_daemon.out 2>&1 < /dev/null &" % BIN, shell=True, cwd=FT); time.sleep(2.5)
data = open(FT + "/torrents_src/Big8M.torrent", "rb").read()
r = urllib.request.Request(B + "/api/add?name=Big8M.torrent&drive=0&start=1", data=data, method="POST")
t0 = time.time(); json.load(urllib.request.urlopen(r, timeout=20))
done = False
while time.time() - t0 < 90:
    try:
        if items().get("Big8M", {}).get("status") == "complete": done = True; break
    except Exception: pass
    time.sleep(0.5)
took = time.time() - t0
st = json.load(open(FT + "/mix_stats.json"))
now = time.time()
def life(mode):
    out = []
    for p, v in st.items():
        if v["mode"] == mode: out.append(((v["closed"] or now) - v["t0"]) if v["closed"] else None)
    return out
silent = life("silent"); choked = life("choked")
print("silent peers lifetime (s):", [None if x is None else round(x, 1) for x in silent])
print("choked peers lifetime (s):", [None if x is None else round(x, 1) for x in choked])
print("download finished: %s in %.0f s" % (done, took))
check("the download completes", done)
sl = [x for x in silent if x is not None]
check("silent peers are dropped quickly (all that connected: max %.1f s <= 12 s)" % (max(sl) if sl else 0), sl and max(sl) <= 12 and all(x is not None for x in silent if x is not None))
ch = [x for x in choked if x is not None]
check("choked-forever peers are dropped without waiting two minutes (max %.1f s <= 25 s)" % (max(ch) if ch else 0), ch and max(ch) <= 25)
out = open(FT + "/mix_daemon.out").read()
check("no sanitizer report", "ERROR: AddressSanitizer" not in out and "runtime error" not in out)
sh("curl -s -m 3 localhost:18787/quit; sleep 3; pkill -9 -x %s; pkill -f '[p]eers_mix'" % BIN)
print("RESULT: %d/%d passed" % (sum(res), len(res)))
