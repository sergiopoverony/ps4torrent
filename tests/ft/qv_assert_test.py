import urllib.request, urllib.error, json, time, os, shutil, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; U = FT + "/usb0/torrents"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def get(p): return json.load(urllib.request.urlopen(B + p, timeout=15))
def items(): return {i["title"]: i for i in get("/status")["items"]}
def log(): return open(FT + "/log/log.txt").read()
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.25)
    return False
def start(): subprocess.Popen("setsid ./daemon_sel >> qv.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)
def quit_():
    try: urllib.request.urlopen(B + "/quit", timeout=3).read()
    except Exception: pass
    time.sleep(3.5)
def same(rel): return os.path.exists(U + "/downloads/" + rel) and open(FT + "/src/" + rel, "rb").read() == open(U + "/downloads/" + rel, "rb").read()
def bad(): return [l.strip() for l in log().split("\n") if "doing a full check" in l or "does not match" in l or "files changed" in l]

sh("pkill -9 -x daemon_sel; bash seeder_ctl.sh stop; rm -rf usb0 usb1 internal log qv.out; mkdir -p usb0/torrents log; bash seeder_ctl.sh start 0.3 0 0 0 0")
shutil.copy(FT + "/torrents_src/Game One.torrent", U + "/Game One.torrent")
start()
h = items()["Game One"]["hash"]
get("/api/select?hash=%s&skip=1" % h); time.sleep(1.5)
get("/api/resume?hash=" + h)
check("downloading with the middle file deselected", wait(lambda: items()["Game One"]["pct"] >= 82, 90))
print("== pause and resume in the same session (the shared boundary piece is already downloaded)")
get("/api/pause?hash=" + h); time.sleep(1.5); get("/api/resume?hash=" + h); time.sleep(1.5)
check("it resumes WITHOUT a full check: %s" % [l for l in log().split("\n") if "resuming without" in l][-1:], "resuming without a full check" in log() and not bad())
print("== restart the whole program")
get("/api/pause?hash=" + h); time.sleep(0.5); pct0 = items()["Game One"]["pct"]; print("   progress at the restart: %d%%" % pct0); quit_(); start()
check("saved selection is kept after the restart (nskip=1)", items()["Game One"]["nskip"] == 1)
get("/api/resume?hash=" + h)
wait(lambda: "already have" in log(), 40)
time.sleep(1)
check("no full check and no 'does not match the files' after the restart: %s" % bad(), not bad())
print("   --- second-session log (without status lines):")
for l in log().split("\n"):
    if "pieces (" not in l and "http: connection" not in l and l.strip(): print("      " + l[:140])
ev = [l.strip() for l in log().split("\n") if "saved progress" in l or "spot check" in l or "resuming without" in l or "already have" in l]
print("   restart log:", ev)
check("it continues from the saved progress (no re-checking of the data)", any("already have" in l for l in ev) and not bad())
check("the task was NOT finished at the restart (this really is a restart in the middle)", pct0 < 100)
check("it completes", wait(lambda: items()["Game One"]["status"] == "complete", 90))
check("a.bin and c.bin identical, b.bin absent", same("Game One/a.bin") and same("Game One/sub/c.bin") and not os.path.exists(U + "/downloads/Game One/b.bin"))
quit_()
print("RESULT: %d/%d passed" % (sum(res), len(res)))
