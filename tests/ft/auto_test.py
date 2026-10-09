import urllib.request, urllib.error, json, time, os, subprocess, sys
B = "http://127.0.0.1:18787"; R = "/tmp/ft/usb0/torrents"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def get(p): return json.load(urllib.request.urlopen(B + p, timeout=10))
def items(): return {i["title"]: i for i in get("/status")["items"]}
def wait(cond, secs=40):
    t = time.time()
    while time.time() - t < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.4)
    return False
def downloaded_files():
    out = []
    for dp, dn, fn in os.walk(R + "/downloads"):
        out += fn
    return out
def pid(): return subprocess.run(["pgrep", "-x", "daemon_auto"], capture_output=True, text=True).stdout.split()[0]
def cpu_ticks(p):
    f = open("/proc/%s/stat" % p).read().split(")")[-1].split(); return int(f[11]) + int(f[12])   # utime + stime

print("== 1. launch with 4 ready torrents on the drive and autostart OFF (default)")
check("all 4 torrents are listed", wait(lambda: len(items()) == 4))
time.sleep(4)
it = items()
check("every task waits on 'paused' after launch: %s" % {k: v["status"] for k, v in it.items()}, all(v["status"] == "paused" for v in it.values()))
check("nothing was downloaded or created on the drive", downloaded_files() == [])
p = pid(); c0 = cpu_ticks(p); time.sleep(5); c1 = cpu_ticks(p)
check("daemon is idle: %d CPU ticks in 5 s (tick = 10 ms)" % (c1 - c0), (c1 - c0) <= 25)
log = open("/tmp/ft/log/log.txt").read()
check("log explains why: '[waiting: autostart is off]'", log.count("[waiting: autostart is off]") == 4)
check("no torrent check/download started in the log", "checking existing data" not in log and "torrent:" not in log.replace("---- ps4torrentd start ----", ""))

print("== 2. start ONE task manually")
get("/api/resume?hash=" + it["Small"]["hash"])
check("Small completes", wait(lambda: items()["Small"]["status"] == "complete"))
it = items()
check("the other tasks are still paused", all(v["status"] == "paused" for k, v in it.items() if k != "Small"))
check("only Small's files exist", os.listdir(R + "/downloads") == ["Small"])

print("== 3. 'start all'")
get("/api/resume_all")
check("all remaining tasks complete", wait(lambda: all(v["status"] == "complete" for v in items().values()), 60))

print("== 4. settings: autostart on/off through the API")
code = get("/api/config"); check("/api/config shows autostart=false", code.get("autostart") is False)
get("/api/set?autostart=1"); time.sleep(1); check("autostart switched on", get("/api/config").get("autostart") is True)
try: urllib.request.urlopen(B + "/api/set?autostart=2", timeout=5); ok = False
except urllib.error.HTTPError as e: ok = e.code == 400
check("autostart=2 rejected (400)", ok)
small = open("/tmp/ft/torrents_src/Small.torrent", "rb").read()
# новый торрент при включённом автозапуске стартует сам
r = urllib.request.Request(B + "/api/add?name=again.torrent", data=open("/tmp/ft/torrents_src/Big8M.torrent", "rb").read(), method="POST")
d = json.load(urllib.request.urlopen(r, timeout=10)); check("upload reply says autostart=true", d.get("autostart") is True)
check("newly added torrent starts by itself", wait(lambda: any(v["status"] in ("downloading", "checking", "complete") and "again" in k for k, v in items().items()), 20))
get("/api/set?autostart=0"); time.sleep(1)
print("config.txt:", open("/tmp/ft/log/config.txt").read().replace("\n", " | "))
check("config.txt stores autostart", "autostart=0" in open("/tmp/ft/log/config.txt").read())
print("RESULT: %d/%d passed" % (sum(res), len(res)))
