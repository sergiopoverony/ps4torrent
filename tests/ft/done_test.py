import urllib.request, urllib.error, json, time, os, shutil, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; U = FT + "/usb0/torrents"
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
def start(): subprocess.Popen("setsid ./daemon_done >> done.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)
def quit_():
    try: urllib.request.urlopen(B + "/quit", timeout=3).read()
    except Exception: pass
    time.sleep(3.5)
def kill9(): sh("pkill -9 -x daemon_done"); time.sleep(1.5)
def same(rel): return os.path.exists(U + "/downloads/" + rel) and open(FT + "/src/" + rel, "rb").read() == open(U + "/downloads/" + rel, "rb").read()

sh("pkill -9 -x daemon_done; bash seeder_ctl.sh stop; rm -rf usb0 usb1 internal log done.out; mkdir -p usb0/torrents log; bash seeder_ctl.sh start 0 0 0 0 0")
for n in ("Small.torrent", "Game One.torrent"): shutil.copy(FT + "/torrents_src/" + n, U + "/" + n)
start()
print("== 1. two downloads finish (autostart), then the program restarts")
check("both complete", wait(lambda: all(items().get(t, {}).get("status") == "complete" for t in ("Small", "Game One")), 40))
check("their .torrent files moved to complete/", os.path.exists(U + "/complete/Small.torrent") and os.path.exists(U + "/complete/Game One.torrent"))
quit_(); start()
it = items()
check("AFTER THE RESTART both finished downloads are still in the list: %s" % sorted((k, v["status"], v["pct"]) for k, v in it.items()), set(it) == {"Small", "Game One"} and all(v["status"] == "complete" and v["pct"] == 100 for v in it.values()))
check("they show their real sizes (Game One %d)" % it["Game One"]["size"], it["Game One"]["size"] == 3600001)
check("log says they were finished earlier", log().count("finished earlier") == 2)

print("== 2. a download with deselected files: the selection is remembered after completion; hard kill this time")
c, j = add("Big8M.torrent", "&start=1&skip=1")
check("Big8M added with the second file deselected", c == 200 and j.get("ok"))
check("it completes", wait(lambda: items().get("Big8M", {}).get("status") == "complete", 60))
kill9(); start()
b = items().get("Big8M", {})
check("after kill -9: Big8M is listed, complete, 1 of 2 files, size of the wanted part (%s)" % b, b.get("status") == "complete" and b.get("nfiles") == 2 and b.get("nskip") == 1 and b.get("size") == 33554432)
check("exactly three items, no duplicates", len(items()) == 3)

print("== 3. remove (without files) and remove + files")
hs, hg = items()["Small"]["hash"], items()["Game One"]["hash"]
get("/api/delete?hash=" + hs)
check("Small disappears from the list", wait(lambda: "Small" not in items(), 10))
check("its .torrent went to removed/, the downloaded file is KEPT", wait(lambda: os.path.exists(U + "/removed/Small.torrent"), 5) and same("Small/only.bin"))
get("/api/delete?files=1&hash=" + hg)
check("Game One disappears and its files are deleted", wait(lambda: "Game One" not in items() and not os.path.exists(U + "/downloads/Game One"), 20))
quit_(); start()
check("after another restart neither of them comes back: %s" % sorted(items()), sorted(items()) == ["Big8M"])

print("== 4. the same torrent is added again: it starts over with ALL files (the old selection must not apply)")
shutil.copy(FT + "/torrents_src/Big8M.torrent", U + "/Big8M.torrent")
check("log: re-added finished torrent", wait(lambda: "re-added finished torrent" in log(), 20))
check("it downloads everything: size 35000000, no deselected files", wait(lambda: items().get("Big8M", {}).get("size") == 35000000 and items()["Big8M"]["nskip"] == 0, 20))
check("it completes and BOTH files are identical", wait(lambda: items().get("Big8M", {}).get("status") == "complete", 60) and same("Big8M/big.bin") and same("Big8M/extra/more.bin"))
check("still one entry", len(items()) == 1)

print("== 5. memory limit: 120 old finished torrents in complete/")
quit_()
def mk(n):
    name = b"t%03d" % n
    info = b"d6:lengthi10e4:name%d:%s12:piece lengthi16384e6:pieces20:%s" % (len(name), name, bytes([n]) * 20) + b"e"
    return b"d8:announce22:http://127.0.0.1:1/ann4:info" + info + b"e"
for n in range(120):
    p = U + "/complete/t%03d.torrent" % n
    open(p, "wb").write(mk(n)); os.utime(p, (1700000000 + n * 100, 1700000000 + n * 100))     # t119 самый свежий
start()
time.sleep(6)
it = items(); done = [k for k, v in it.items() if v["status"] == "complete"]
check("no more than 100 finished torrents are shown (%d)" % len(done), len(done) <= 100)
check("the NEWEST ones are shown (t119 yes, t000 no)", "t119" in it and "t000" not in it)
check("log explains the limit", "not all finished torrents from complete/ are shown" in log())
check("the files themselves stay in the folder", len([f for f in os.listdir(U + "/complete") if f.startswith("t")]) == 120)
quit_()
print("RESULT: %d/%d passed" % (sum(res), len(res)))
