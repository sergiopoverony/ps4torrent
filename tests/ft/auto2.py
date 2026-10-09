import urllib.request, json, time, os, subprocess, shutil, sys
B = "http://127.0.0.1:18787"; R = "/tmp/ft/usb0/torrents"; FT = "/tmp/ft"
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
        time.sleep(0.3)
    return False
def sh(cmd): subprocess.run(cmd, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def start_daemon(tag):
    subprocess.Popen("setsid ./daemon_auto > %s.out 2>&1 < /dev/null &" % tag, shell=True, cwd=FT); time.sleep(2.5)
def quit_daemon():
    try: urllib.request.urlopen(B + "/quit", timeout=3).read()
    except Exception: pass
    time.sleep(3)
def log(): return open(FT + "/log/log.txt").read()
H = "af60bf1b080ed007e1c391e5fb552366e81194f0"            # Big8M

sh("pkill -9 -x daemon_auto; bash seeder_ctl.sh stop; rm -rf usb0 usb1 log served.txt; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/")
open(FT + "/log/config.txt", "w").write("autostart=1\n")
print("== 1. first session with autostart ON: download part of a piece, then stop the program")
sh("bash seeder_ctl.sh start 0.002 300 0 0 0")
start_daemon("s1"); time.sleep(12); quit_daemon()
check("partial data saved (a .part file exists)", any(f.endswith(".part") for f in os.listdir(R + "/.state")))

print("== 2. new session (autostart OFF): the task must NOT start and must not touch the drive")
open(FT + "/log/config.txt", "w").write("autostart=0\nresume_session=0\n")      # проверяем именно правило автозапуска, без памяти о сессии
sh("bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0 0 0 0 0; rm -f served.txt")
size_before = os.path.getsize(R + "/downloads/Big8M/big.bin")
start_daemon("s2"); time.sleep(6)
it = items()
check("task waits on 'paused': %s" % {k: v["status"] for k, v in it.items()}, it["Big8M"]["status"] == "paused")
l = log()
check("no check, no restore, no download started yet", "checking existing data" not in l and "restored" not in l and "announce" not in l)
check("the file on the drive was not touched", os.path.getsize(R + "/downloads/Big8M/big.bin") == size_before)

print("== 3. the user presses resume: continues from the saved partial piece")
get("/api/resume?hash=" + H)
check("completes", wait(lambda: items()["Big8M"]["status"] == "complete", 40))
check("saved blocks were reused ('restored 1 partial pieces')", "restored 1 partial pieces" in log())
ok = all(open("/tmp/ft/src/Big8M/%s" % f, "rb").read() == open(R + "/downloads/Big8M/%s" % f, "rb").read() for f in ("big.bin", "extra/more.bin"))
check("files identical", ok)
quit_daemon()

print("== 4. 'pause all' with two tasks running")
sh("pkill -9 -x daemon_auto; bash seeder_ctl.sh stop; rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Many.torrent torrents_src/Big8M.torrent usb0/torrents/")
open(FT + "/log/config.txt", "w").write("autostart=0\n")
sh("bash seeder_ctl.sh start 0.03 0 0 0 0")
start_daemon("s4"); time.sleep(1.5)
get("/api/resume_all")
check("both tasks running", wait(lambda: all(v["status"] in ("downloading", "checking") for v in items().values()), 15))
time.sleep(4)
get("/api/pause_all"); time.sleep(1.5)
it = items(); p1 = {k: v["done"] for k, v in it.items()}
check("both tasks paused: %s" % {k: v["status"] for k, v in it.items()}, all(v["status"] == "paused" for v in it.values()))
time.sleep(4); it2 = items()
check("no progress and no speed after pause", all(it2[k]["done"] == p1[k] and it2[k]["speed_kb"] == 0 for k in it2))

print("== 5. drive pulled out and put back while a task runs (it was running before: must continue by itself)")
get("/api/resume?hash=" + H); time.sleep(5)
st = items()["Big8M"]["status"]; check("Big8M is downloading (%s)" % st, st in ("downloading", "checking"))
shutil.move(R.rsplit("/", 1)[0], "/tmp/ft/usb_gone"); time.sleep(13)
check("task goes 'offline' while the drive is away", items()["Big8M"]["status"] == "offline")
shutil.move("/tmp/ft/usb_gone", R.rsplit("/", 1)[0]); time.sleep(14)
st = items()["Big8M"]["status"]
check("after the drive returns the task continues by itself (%s), not left paused" % st, st in ("downloading", "checking", "complete"))
quit_daemon()
print("RESULT: %d/%d passed" % (sum(res), len(res)))
