# Медленное удаление файлов (каждый unlink 5 с): программа остаётся отзывчивой, закачка другой раздачи идёт.
import urllib.request, json, time, os, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def get(p, t=40): return urllib.request.urlopen(B + p, timeout=t).read()
def items(): return {i["title"]: i for i in json.loads(get("/status"))["items"]}
def log(): return open(FT + "/log/log.txt").read()
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.2)
    return False
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
sh("pkill -9 -x daemon_host; bash seeder_ctl.sh stop; rm -f served.txt write_cost unlink_cost; bash seeder_ctl.sh start 0 0 0 0 0; rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log; cp 'torrents_src/Game One.torrent' torrents_src/Small.torrent usb0/torrents/")
subprocess.Popen("setsid ./daemon_host > sd.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)
check("both finish", wait(lambda: all(items().get(t, {}).get("status") == "complete" for t in ("Game One", "Small")), 40))
open(FT + "/unlink_cost", "w").write("5")
h = items()["Game One"]["hash"]
t0 = time.time(); get("/api/delete?files=1&hash=" + h, 15); dt = time.time() - t0
check("delete answered quickly (%.2f s)" % dt, dt < 3)
lat = []
t1 = time.time()
while time.time() - t1 < 10:
    s0 = time.time(); get("/status", 10); lat.append(time.time() - s0); time.sleep(0.5)
check("the program stays responsive during the slow deletion (worst /status %.2f s)" % max(lat), max(lat) < 1.0)
check("deleting 3 files at 5 s each is NOT finished yet after 10 s", "delete finished" not in log())
check("the other task can be removed meanwhile (command applied within 3 s)", (get("/api/delete?hash=" + items()["Small"]["hash"], 8) or True) and wait(lambda: "Small" not in items(), 4))
check("deletion finishes by itself with all 3 files removed", wait(lambda: "files removed 3" in log(), 30))
check("no slow-loop warning", "slow main loop: deletions" not in log())
out = open(FT + "/sd.out").read()
check("no sanitizer report", "ERROR: AddressSanitizer" not in out and "runtime error" not in out)
sh("curl -s -m 3 localhost:18787/quit; sleep 3; pkill -9 -x daemon_host; bash seeder_ctl.sh stop; rm -f unlink_cost")
print("RESULT: %d/%d passed" % (sum(res), len(res)))
