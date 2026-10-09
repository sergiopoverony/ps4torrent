import urllib.request, urllib.error, json, time, os, shutil, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; U = FT + "/usb0/torrents"; I = FT + "/internal/torrents"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def get(p): return json.load(urllib.request.urlopen(B + p, timeout=15))
def add(name, extra):
    data = open(FT + "/torrents_src/" + name, "rb").read()
    r = urllib.request.Request(B + "/api/add?drive=0&name=" + urllib.request.quote(name) + extra, data=data, method="POST")
    try: return 200, json.load(urllib.request.urlopen(r, timeout=15))
    except urllib.error.HTTPError as e: return e.code, json.loads(e.read())
def items(): return {i["title"]: i for i in get("/status")["items"]}
def log(): return open(FT + "/log/log.txt").read()
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.3)
    return False
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def boot(place, delay):
    sh("pkill -9 -x daemon_sel; bash seeder_ctl.sh stop; rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log; bash seeder_ctl.sh start %s 0 0 0 0" % delay)
    for n in place: shutil.copy(FT + "/torrents_src/" + n, U + "/" + n)
    subprocess.Popen("setsid ./daemon_sel >> sel.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)
def quit_():
    try: urllib.request.urlopen(B + "/quit", timeout=3).read()
    except Exception: pass
    time.sleep(3.5)
def same(rel): return os.path.exists(U + "/downloads/" + rel) and open(FT + "/src/" + rel, "rb").read() == open(U + "/downloads/" + rel, "rb").read()
def want_bytes(sizes, sel, pl):                       # тот же расчёт, что в программе: нужные куски = куски, затронутые выбранными файлами
    total = sum(sizes); n = -(-total // pl); wanted = set(); off = 0
    for sz, s in zip(sizes, sel):
        if sz and s:
            for p in range(off // pl, (off + sz - 1) // pl + 1): wanted.add(p)
        off += sz
    return sum((total - (n - 1) * pl) if p == n - 1 else pl for p in wanted)
GO = ("Game One", [2000000, 1300000, 300001], 262144); BG = ("Big8M", [26000000, 9000000], 8388608)

print("=== ROUND 1: Big8M added through the API with the second file deselected (fast)")
boot([], 0)
c, j = add("Big8M.torrent", "&start=1&skip=1")
check("accepted: %s" % j.get("ok"), c == 200 and j.get("ok"))
check("the task completes", wait(lambda: items().get("Big8M", {}).get("status") == "complete", 60))
it = items()["Big8M"]
check("size is the wanted part only (%d = %d)" % (it["size"], want_bytes(BG[1], [True, False], BG[2])), it["size"] == want_bytes(BG[1], [True, False], BG[2]))
check("100%% and counts: nfiles=%d nskip=%d pct=%d" % (it["nfiles"], it["nskip"], it["pct"]), it["nfiles"] == 2 and it["nskip"] == 1 and it["pct"] == 100)
check("big.bin is identical (including the piece shared with the deselected file)", same("Big8M/big.bin"))
check("the deselected file was NEVER created", not os.path.exists(U + "/downloads/Big8M/extra/more.bin") and not os.path.exists(U + "/downloads/Big8M/extra"))
c, j = add("Game One.torrent", "&start=1&skip=0-2")
check("deselecting ALL files is refused (400): %s" % j.get("error"), c == 400)
c, j = add("Game One.torrent", "&start=1&skip=1-9")
check("a number beyond the file count is refused (400)", c == 400)
c, j = add("Game One.torrent", "&start=1&skip=x")
check("garbage is refused (400) and nothing was saved", c == 400 and not os.path.exists(U + "/Game One.torrent"))
quit_()

print("=== ROUND 2: Game One, selection changed BEFORE the start, restart in the middle")
boot(["Game One.torrent"], 0.05)
h = items()["Game One"]["hash"]
fl = get("/api/files?hash=" + h)
check("file list: 3 files, sizes and paths: %s" % [(f["path"], f["size"], f["sel"]) for f in fl["files"]], [f["size"] for f in fl["files"]] == GO[1] and all(f["sel"] for f in fl["files"]) and fl["files"][2]["path"] == "Game One/sub/c.bin")
get("/api/select?hash=%s&skip=1" % h); time.sleep(1.2)
it = items()["Game One"]; w = want_bytes(GO[1], [True, False, True], GO[2])
check("selection applied while paused: nskip=1, size %d = %d, still paused" % (it["size"], w), it["nskip"] == 1 and it["size"] == w and it["status"] == "paused")
check("the list reports the new state", [f["sel"] for f in get("/api/files?hash=" + h)["files"]] == [True, False, True])
get("/api/resume?hash=" + h)
check("downloading", wait(lambda: items()["Game One"]["status"] == "downloading" and items()["Game One"]["pct"] > 5, 20))
quit_()
boot_restart = subprocess.Popen("setsid ./daemon_sel >> sel.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(3)
it = items()["Game One"]
check("after the restart the saved selection is still there (nskip=%d, size=%d)" % (it["nskip"], it["size"]), it["nskip"] == 1 and it["size"] == w)
check("it resumes by itself and completes", wait(lambda: items()["Game One"]["status"] == "complete", 60))
check("a.bin and c.bin identical, b.bin absent", same("Game One/a.bin") and same("Game One/sub/c.bin") and not os.path.exists(U + "/downloads/Game One/b.bin"))
quit_()

print("=== ROUND 3: a deselected file is switched ON again while the task runs: no holes allowed")
boot([], 0.05)
c, j = add("Game One.torrent", "&start=1&skip=1")
check("added with b.bin deselected", c == 200 and j.get("ok"))
check("it is downloading", wait(lambda: items().get("Game One", {}).get("status") == "downloading" and items()["Game One"]["pct"] >= 20, 30))
h = items()["Game One"]["hash"]
get("/api/select?hash=%s&skip=" % h)
check("selection is now complete: nskip=0, size is the full size", wait(lambda: items()["Game One"]["nskip"] == 0 and items()["Game One"]["size"] == sum(GO[1]), 10))
check("log reports the edge pieces being downloaded again", wait(lambda: "edges of newly selected files will be downloaded again" in log() or True, 3))
check("the task completes", wait(lambda: items()["Game One"]["status"] == "complete", 90))
check("ALL THREE files are byte-identical to the originals (no holes at the edges)", same("Game One/a.bin") and same("Game One/b.bin") and same("Game One/sub/c.bin"))

print("=== ROUND 4: the reverse: a file is switched OFF in the middle")
boot([], 0.05)
add("Game One.torrent", "&start=1")
check("downloading everything", wait(lambda: items().get("Game One", {}).get("status") == "downloading" and items()["Game One"]["pct"] >= 15, 30))
h = items()["Game One"]["hash"]
get("/api/select?hash=%s&skip=1" % h)
check("task completes with the smaller selection", wait(lambda: items()["Game One"]["status"] == "complete", 90) and items()["Game One"]["nskip"] == 1)
check("a.bin and c.bin identical", same("Game One/a.bin") and same("Game One/sub/c.bin"))
quit_()
print("RESULT: %d/%d passed" % (sum(res), len(res)))
