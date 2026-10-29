# Отдача по умолчанию включена (1.8.4.0) и крест в приложении: включить/выключить отдачу одной завершённой раздачи
# (бинарник daemon_seed: UPLOAD_DEFAULT_ON=1, SEED_MAX=1, чтобы проверить, что включённая вручную раздача получает место).
import os, sys, time, json, subprocess, urllib.request, urllib.error
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; BIN = "daemon_seed"
res = []
def check(name, ok): ok = bool(ok); res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def getq(p):
    try: return 200, json.load(urllib.request.urlopen(B + p, timeout=15))
    except urllib.error.HTTPError as e: return e.code, json.loads(e.read())
def add(name):
    data = open(FT + "/torrents_src/" + name, "rb").read()
    r = urllib.request.Request(B + "/api/add?drive=0&start=1&name=" + urllib.request.quote(name), data=data, method="POST")
    return json.load(urllib.request.urlopen(r, timeout=15))
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
def log(): return open(FT + "/log/log.txt").read()
def boot(wipe=True, cfg=None):
    if wipe: sh("pkill -9 -x %s; bash seeder_ctl.sh stop; rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log; bash seeder_ctl.sh start 0 0 0 0" % BIN)
    else: sh("pkill -9 -x %s" % BIN)
    if cfg is not None: open(FT + "/log/config.txt", "w").write(cfg)
    subprocess.Popen("setsid ./%s >> seedtoggle.out 2>&1 < /dev/null &" % BIN, shell=True, cwd=FT); time.sleep(2.5)
def quit_():
    try: urllib.request.urlopen(B + "/quit", timeout=3).read()
    except Exception: pass
    wait(lambda: subprocess.run("pgrep -x %s" % BIN, shell=True, stdout=subprocess.DEVNULL).returncode != 0, 15)

open(FT + "/seedtoggle.out", "w").close()
print("=== upload is on by default", flush=True)
boot()
check("no config file: upload on", getq("/api/config")[1]["upload"] is True)
quit_()
boot(wipe=False, cfg="max_parallel=3\nupload=0\nupload_kb=32\n")
check("old config from before 1.8.4.0 (upload=0 without upload_v): upload on", getq("/api/config")[1]["upload"] is True)
getq("/api/set?upload=0")
time.sleep(1)
cfg = open(FT + "/log/config.txt").read()
check("saving writes upload_v=2", "upload=0" in cfg and "upload_v=2" in cfg)
quit_()
boot(wipe=False)
check("a choice saved by this version is kept (off stays off)", getq("/api/config")[1]["upload"] is False)
getq("/api/set?upload=1")
time.sleep(1)

print("=== cross on a finished task", flush=True)
add("Game One.torrent"); add("Small.torrent")
check("both complete", wait(lambda: items()["Game One"]["status"] == "complete" and items()["Small"]["status"] == "complete", 90))
check("one of them is seeding (SEED_MAX=1)", wait(lambda: sum(1 for i in items().values() if i["seeding"]) == 1, 15))
seeder = [t for t, i in items().items() if i["seeding"]][0]; other = "Small" if seeder == "Game One" else "Game One"
h = items()[seeder]["hash"]; ho = items()[other]["hash"]
check("seed_off is false by default", items()[seeder]["seed_off"] is False)
c, d = getq("/api/seed?hash=%s&on=0" % h)
check("seed off accepted", c == 200 and d["ok"])
check("seeding stops, seed_off true, status complete", wait(lambda: items()[seeder]["seeding"] is False and items()[seeder]["seed_off"] is True and items()[seeder]["status"] == "complete", 5))
check("log says stopped by the user", "stopped by the user" in log())
check("the other task takes the free place", wait(lambda: items()[other]["seeding"] is True, 10))
time.sleep(3)
check("the switched-off task does not come back by itself", items()[seeder]["seeding"] is False)
quit_()
boot(wipe=False)
check("after restart: still off", wait(lambda: items()[seeder]["seed_off"] is True, 10) and wait(lambda: items()[other]["seeding"], 15) and items()[seeder]["seeding"] is False)
check("noseed.txt keeps the hash", h in open(FT + "/log/noseed.txt").read())
c, d = getq("/api/seed?hash=%s&on=1" % h)
check("seed on: starts at once and takes the place of the other one", wait(lambda: items()[seeder]["seeding"] is True and items()[seeder]["seed_off"] is False, 5) and items()[other]["seeding"] is False)
check("log: turn passes to a task the user started", "turn passes to a task the user started" in log())
check("noseed.txt no longer has it", h not in open(FT + "/log/noseed.txt").read())
c, d = getq("/api/seed?hash=%s&on=0" % ho)
time.sleep(1)
check("the other one switched off while waiting: seed_off true, not seeding", items()[other]["seed_off"] is True and items()[other]["seeding"] is False)
check("bad requests: short hash -> 400, bad on -> 400", getq("/api/seed?hash=abc&on=1")[0] == 400 and getq("/api/seed?hash=%s&on=2" % h)[0] == 400)
getq("/api/delete?hash=" + ho)
time.sleep(1.5)
check("deleting a task removes it from noseed.txt", ho not in open(FT + "/log/noseed.txt").read())
getq("/api/set?upload=0")
check("upload off: seeding stops", wait(lambda: items()[seeder]["seeding"] is False, 5))
getq("/api/seed?hash=%s&on=1" % h); time.sleep(1.5)
check("seed on while upload is off: nothing starts", items()[seeder]["seeding"] is False)
quit_()
sh("bash seeder_ctl.sh stop")
out = open(FT + "/seedtoggle.out").read()
check("no sanitizer errors", "ERROR: AddressSanitizer" not in out and "runtime error" not in out)
print("RESULT %d/%d" % (sum(res), len(res)))
