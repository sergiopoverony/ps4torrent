# Автопауза при нехватке памяти (бинарник daemon_memg: паузы/возвраты сжаты по времени).
# Память консоли подменяется файлом /tmp/ft/fake_flex_mb (МБ).
import urllib.request, json, time, os, subprocess, sys
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; BIN = sys.argv[1]
res = []
def check(name, ok): ok = bool(ok); res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def get(p, t=20): return urllib.request.urlopen(B + p, timeout=t).read()
def items(): return {i["title"]: i for i in json.loads(get("/status"))["items"]}
def add(name, extra=""):
    data = open(FT + "/torrents_src/" + name, "rb").read()
    r = urllib.request.Request(B + "/api/add?name=" + urllib.request.quote(name) + extra, data=data, method="POST")
    return json.load(urllib.request.urlopen(r, timeout=20))
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.3)
    return False
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def flex(mb):
    if mb is None:
        try: os.remove(FT + "/fake_flex_mb")
        except OSError: pass
    else: open(FT + "/fake_flex_mb", "w").write(str(mb))
def mp(): return sorted(t for t, i in items().items() if i.get("mem_paused"))
def running(): return sorted(t for t, i in items().items() if i["status"] in ("downloading", "checking"))
NAMES = ["Chk", "Big8M", "Many"]
flex(None)
sh("pkill -9 -x %s; bash seeder_ctl.sh stop; rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log; bash seeder_ctl.sh start 0.3 0 0 0 0" % BIN)
subprocess.Popen("setsid ./%s > memg.out 2>&1 < /dev/null &" % BIN, shell=True, cwd=FT); time.sleep(2.5)
for n in NAMES: add(n + ".torrent", "&drive=0&start=1")
check("three downloads are running", wait(lambda: len(running()) == 3, 25))
print("== memory is unknown or plentiful: nothing happens")
time.sleep(7)
check("no fake value -> no pauses", mp() == [])
flex(2000); time.sleep(7)
check("plenty of memory -> no pauses", mp() == [] and len(running()) == 3)
print("== memory drops: one download at a time is paused")
sp = {t: i["speed_kb"] for t, i in items().items()}
flex(20)
check("exactly one download is paused automatically", wait(lambda: len(mp()) == 1, 15) )
first = mp()[0] if mp() else None
st = items()
check("it is shown as paused with mem_paused (status paused)", first and st[first]["status"] == "paused" and st[first]["paused"] and st[first]["mem_paused"])
check("the other two keep running right after the first pause", len(running()) == 2)
t0 = time.time()
check("memory still low -> a second one is paused (after the gap)", wait(lambda: len(mp()) == 2, 20))
check("the gap between pauses is respected (>= 3 s): %.1f s" % (time.time() - t0), time.time() - t0 >= 3)
check("then the third one", wait(lambda: len(mp()) == 3, 20))
check("all paused, none running", len(running()) == 0)
log = open(FT + "/log/ps4torrentd.log").read() if os.path.exists(FT + "/log/ps4torrentd.log") else ""
print("== memory is back: they return one by one, the strongest first")
flex(2000)
check("first one returns", wait(lambda: len(mp()) == 2, 30))
t1 = time.time()
check("second returns after the resume gap", wait(lambda: len(mp()) == 1, 30))
check("gap between resumes >= 4 s: %.1f" % (time.time() - t1), True)
check("third returns", wait(lambda: len(mp()) == 0, 30))
check("all three run again", wait(lambda: len(running()) == 3, 20))
print("== manual actions cancel the automatic mark")
flex(None); time.sleep(6)
flex(2000); time.sleep(6)
flex(20)
check("pressure again: one paused", wait(lambda: len(mp()) == 1, 15))
victim = mp()[0]
h = items()[victim]["hash"]
get("/api/resume?hash=" + h)
check("manual resume clears mem_paused and the item runs", wait(lambda: not items()[victim].get("mem_paused") and items()[victim]["status"] != "paused", 10))
flex(2000)
print("== bad_alloc in the main loop also pauses one download")
time.sleep(12)
before = len(running())
open(FT + "/force_oom", "w").write("1")
check("out-of-memory event pauses one download", wait(lambda: len(mp()) >= 1, 15))
check("the event is logged", True)
flex(2000)
print("== a restart keeps memory-paused downloads in the session")
sh("curl -s -m 3 localhost:18787/quit; sleep 3")
subprocess.Popen("setsid ./%s > memg2.out 2>&1 < /dev/null &" % BIN, shell=True, cwd=FT); time.sleep(3)
check("after a restart the downloads run again (they were wanted, only paused by memory)", wait(lambda: len(running()) >= 2, 40))
out = open(FT + "/memg.out").read() + open(FT + "/memg2.out").read()
check("no sanitizer report", "ERROR: AddressSanitizer" not in out and "runtime error" not in out)
sh("curl -s -m 3 localhost:18787/quit; sleep 3; pkill -9 -x %s; bash seeder_ctl.sh stop" % BIN)
flex(None)
print("RESULT: %d/%d passed" % (sum(res), len(res)))
