import urllib.request, json, time, os, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; R = FT + "/usb0/torrents"
H = "af60bf1b080ed007e1c391e5fb552366e81194f0"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def status(t=40): return json.load(urllib.request.urlopen(B + "/status", timeout=t))["items"][0]
def get(p, t=40): return urllib.request.urlopen(B + p, timeout=t).read()
def log(): return open(FT + "/log/log.txt").read()
def served(): 
    try: return int(open(FT + "/served.txt").read())
    except Exception: return 0
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.2)
    return False
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def identical():
    return all(open(FT + "/src/Big8M/" + f, "rb").read() == open(R + "/downloads/Big8M/" + f, "rb").read() for f in ("big.bin", "extra/more.bin"))
def start(seeder="0.01 0 0 0 0", cost="1200 330"):
    sh("pkill -9 -x daemon_host; bash seeder_ctl.sh stop; rm -f served.txt write_cost; bash seeder_ctl.sh start %s; rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/" % seeder)
    open(FT + "/write_cost", "w").write(cost)
    subprocess.Popen("setsid ./daemon_host > wr.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)

print("== C. PAUSE while a verified piece is still being written to a slow drive: nothing may be lost")
start()
wait(lambda: status()["done"] == 0 and time.time() > 0, 1)
time.sleep(6.5)                                           # первый кусок (8 МБ) скачан и пишется (~4 с на этой "флешке")
t = time.time(); get("/api/pause?hash=" + H); dt = time.time() - t
print("   pause answered in %.2f s" % dt)
check("pause does not wait for the slow drive (%.2f s)" % dt, dt < 1.0)
check("status shows paused immediately", status()["status"] == "paused")
check("the piece that was being written is accounted for once the drive finishes it (done=%s MB)" % "?", wait(lambda: status()["done"] >= 8 * (1 << 20), 15))
check("log: progress saved after the writes finished", wait(lambda: "disk writes of" in log() and "progress saved" in log(), 5))
open(FT + "/write_cost", "w").write("0 0"); get("/api/resume?hash=" + H)
check("after resume the task completes", wait(lambda: status()["status"] == "complete", 40))
sv = served()
check("the already verified piece was NOT downloaded again (blocks served %d, a full download is ~2137)" % sv, sv <= 2137 + 80)
check("files identical", identical())
sh("curl -s -m 3 localhost:18787/quit; sleep 2")

print("== D. the drive is HUNG (every write call blocks for 8 s): the program must stay responsive")
start(cost="8000 0")
time.sleep(7)                                             # кусок скачан, запись зависла
t = time.time()
get("/api/pause?hash=" + H)
pt = time.time() - t
print("   pause took %.2f s" % pt)
check("pause does not wait for the hung drive at all (%.2f s)" % pt, pt < 1.0)
l = log()
check("log: the task is stopped, progress is saved when the writes finish", "are still being written to the drive: the task is stopped" in l)
lat = []
for _ in range(5):
    t = time.time(); get("/api/set?max_parallel=3"); lat.append(time.time() - t); time.sleep(0.4)
check("the program is responsive while the drive hangs (worst command latency %.2f s)" % max(lat), max(lat) < 1.0)
check("the page and /status work", status()["status"] == "paused")
check("after the hung write finishes the stopped task is released and its progress saved", wait(lambda: "progress saved" in log(), 90))
os.remove(FT + "/write_cost"); get("/api/resume?hash=" + H)
check("the task resumes and completes with identical files", wait(lambda: status()["status"] == "complete", 60) and identical())
sh("curl -s -m 3 localhost:18787/quit; sleep 2")
print("RESULT: %d/%d passed" % (sum(res), len(res)))
