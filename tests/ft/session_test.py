import urllib.request, json, time, os, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; SF = FT + "/log/session.txt"
HB = "6b5d373445b803f9bb3630e88ae74f1617438cc2"                 # Big8M
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def status(): return json.load(urllib.request.urlopen(B + "/status", timeout=8))
def items(): return {i["title"]: i for i in status()["items"]}
def get(p): return json.load(urllib.request.urlopen(B + p, timeout=10))
def log(): return open(FT + "/log/log.txt").read()
def sess(): return sorted(l.split()[1] for l in open(SF).read().split("\n") if l.startswith("want ")) if os.path.exists(SF) else []
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.25)
    return False
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def start(): subprocess.Popen("setsid ./daemon_sess > sess.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.2)
def quit_():
    try: urllib.request.urlopen(B + "/quit", timeout=3).read()
    except Exception: pass
    time.sleep(3.5)
def active(s): return s in ("downloading", "checking")

sh("pkill -9 -x daemon_sess; bash seeder_ctl.sh stop; rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log; bash seeder_ctl.sh start 0.02 0 0 0 0; cp torrents_src/Big8M.torrent torrents_src/Many.torrent 'torrents_src/Game One.torrent' usb0/torrents/")
start()
it = items(); hB, hM, hG = it["Big8M"]["hash"], it["Many"]["hash"], it["Game One"]["hash"]
check("fresh start: every task is paused, no session file yet", all(v["status"] == "paused" for v in it.values()) and sess() == [])

print("== 1. the user starts two tasks: they are remembered")
get("/api/resume?hash=" + hB); get("/api/resume?hash=" + hM)
check("session file lists exactly the two running tasks (within a few seconds)", wait(lambda: sess() == sorted([hB, hM]), 10))
quit_()
check("a normal stop keeps the list (the tasks are still wanted)", sess() == sorted([hB, hM]))

print("== 2. restart: they come back by themselves, ONE BY ONE")
start()
check("log: 2 tasks were running when the program stopped", "session: 2 task(s) were running" in log())
check("both are scheduled ('resuming: ... starts in N s') and the third stays paused", log().count("[resuming: it was downloading before the restart") == 2 and items()["Game One"]["status"] == "paused")
t0 = time.time(); first = second = None
while time.time() - t0 < 25 and (first is None or second is None):
    it = items(); a, b = active(it["Big8M"]["status"]), active(it["Many"]["status"])
    if (a or b) and first is None: first = time.time() - t0
    if a and b and second is None: second = time.time() - t0
    time.sleep(0.4)
check("the first resumes after the first delay (3 s in this build): %.1f s" % (first or -1), first is not None and 1.5 <= first <= 9)
check("the second one starts later, not together with the first (%.1f s after the first)" % ((second - first) if first and second else -1), first is not None and second is not None and second - first >= 3.5)
check("the never-started task is still paused", items()["Game One"]["status"] == "paused")

print("== 3. the user pauses one, then the program is killed hard (kill -9)")
get("/api/pause?hash=" + hB)
check("the list shrinks to the one still running", wait(lambda: sess() == [hM], 10))
sh("pkill -9 -x daemon_sess"); time.sleep(1.5)
start()
it = items()
check("after kill -9 the paused one stays paused, the running one is resumed", it["Big8M"]["status"] == "paused" and it["Many"]["status"] != "paused")

print("== 4. the setting 'resume interrupted downloads' switched OFF")
get("/api/set?resume_session=0")
check("config reports it off and stores it", wait(lambda: get("/api/config")["resume_session"] is False and "resume_session=0" in open(FT + "/log/config.txt").read(), 6))
quit_(); start()
check("nothing is resumed after the restart", all(v["status"] == "paused" for v in items().values()) and "[resuming:" not in log())
get("/api/set?resume_session=1"); time.sleep(1)

print("== 5. 'pause all' forgets everything")
get("/api/resume_all"); time.sleep(3)
get("/api/pause_all")
check("the list becomes empty", wait(lambda: sess() == [], 10))
quit_(); start()
check("after the restart nothing runs", all(v["status"] == "paused" for v in items().values()))

print("== 6. finished tasks are forgotten")
sh("bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0 0 0 0 0")
get("/api/resume?hash=" + hG)
check("the task completes", wait(lambda: items().get("Game One", {}).get("status") == "complete", 40))
check("its entry is removed from the list", wait(lambda: hG not in sess(), 10))

print("== 7. deleting a running task removes it from the list")
sh("bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0.02 0 0 0 0")
get("/api/resume?hash=" + hM); time.sleep(4)
check("running task is on the list", hM in sess())
get("/api/delete?hash=" + hM)
check("after deleting it is gone from the list", wait(lambda: hM not in sess(), 10))
quit_()
print("RESULT: %d/%d passed" % (sum(res), len(res)))
