import urllib.request, urllib.error, json, time, os, shutil, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; U = FT + "/usb0/torrents"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def get(p, t=10): return json.load(urllib.request.urlopen(B + p, timeout=t))
def st(): return get("/status")
def items(): return {i["title"]: i for i in st()["items"]}
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
def boot(binary, torrents=(), delay=None, fake=()):
    sh("pkill -9 -x daemon_mem daemon_old daemon_mem_plain daemon_old_plain; bash seeder_ctl.sh stop; rm -rf usb0 usb1 internal log force_oom; mkdir -p usb0/torrents log")
    if delay is not None: sh("bash seeder_ctl.sh start %s 0 0 0 0" % delay)
    for n in torrents: shutil.copy(FT + "/torrents_src/" + n, U + "/" + n)
    for name, pieces in fake:
        plen = 4194304
        info = b"d6:lengthi%de4:name%d:%s12:piece lengthi%de6:pieces%d:%se" % (pieces * plen, len(name), name.encode(), plen, 20 * pieces, os.urandom(20 * pieces))
        open(U + "/" + name + ".torrent", "wb").write(b"d8:announce22:http://127.0.0.1:1/ann4:info" + info + b"e")
    subprocess.Popen("setsid ./%s >> mem.out 2>&1 < /dev/null &" % binary, shell=True, cwd=FT); time.sleep(2.5)
def quit_():
    try: urllib.request.urlopen(B + "/quit", timeout=3).read()
    except Exception: pass
    time.sleep(3)
def same(rel): return os.path.exists(U + "/downloads/" + rel) and open(FT + "/src/" + rel, "rb").read() == open(U + "/downloads/" + rel, "rb").read()

print("=== A. heap use on the three big swarms (God of War 25953, Tekken 7 11187, Contra 7670 pieces)")
FAKE = [("GoW", 25953), ("Tekken", 11187), ("Contra", 7670)]
heap = {}
for tag, binary in (("old 1.3.0.2", "daemon_old_plain"), ("new 1.4.1.2", "daemon_mem_plain")):
    boot(binary); time.sleep(4); base = st()["heap_kb"]; quit_()
    boot(binary, fake=FAKE)
    wait(lambda: sum(1 for i in st()["items"] if i["status"] in ("downloading", "checking", "queued")) >= 3, 25); time.sleep(12)
    h = st()["heap_kb"]; heap[tag] = (base, h)
    print("   %-12s heap with no tasks %6d KB, with the three big swarms running %6d KB  => %d KB used by them" % (tag, base, h, h - base))
    quit_()
d_old = heap["old 1.3.0.2"][1] - heap["old 1.3.0.2"][0]; d_new = heap["new 1.4.1.2"][1] - heap["new 1.4.1.2"][0]
check("the new version needs much less heap for the three swarms (%d KB instead of %d KB, saved %d KB)" % (d_new, d_old, d_old - d_new), d_new < d_old - 4000)
check("and stays far below the ~10 MB console limit with a big margin (%d KB)" % d_new, d_new < 3000)

print("=== B. piece limit lines, buffer accounting, and normal downloads")
boot("daemon_mem", ["Game One.torrent", "Big8M.torrent"], 0.01)
check("log: small pieces -> up to 64 pieces (16 MB) per task", wait(lambda: "up to 64 pieces of 256 KB (16 MB) are held in memory at once" in log(), 15))
check("log: 8 MB pieces -> 4 pieces (32 MB) per task instead of 1-2", "up to 4 pieces of 8192 KB (32 MB) are held in memory at once" in log())
mx = 0
t0 = time.time()
while time.time() - t0 < 40 and not all(items().get(t, {}).get("status") == "complete" for t in ("Game One", "Big8M")):
    mx = max(mx, st()["buf_kb"]); time.sleep(0.1)
check("buffers were in use while downloading (peak %d KB)" % mx, mx > 0)
check("both tasks complete", all(items().get(t, {}).get("status") == "complete" for t in ("Game One", "Big8M")))
check("all files identical", all(same(f) for f in ("Game One/a.bin", "Game One/b.bin", "Game One/sub/c.bin", "Big8M/big.bin", "Big8M/extra/more.bin")))
check("after completion the buffer counter returns to zero (no leak): %d KB" % st()["buf_kb"], wait(lambda: st()["buf_kb"] < 64, 8))
quit_()

print("=== C. out of memory in the middle of a download: the program must carry on")
boot("daemon_mem", ["Big8M.torrent"], 0.02)
check("downloading", wait(lambda: items().get("Big8M", {}).get("status") == "downloading" and items()["Big8M"]["pct"] >= 15, 30))
open(FT + "/force_oom", "w").write("1")
check("log: out of memory handled, program continues", wait(lambda: "OUT OF MEMORY (std::bad_alloc) #1" in log() and "the program continues" in log(), 8))
check("the page keeps answering", st()["version"] == "1.4.1.3")
check("the download finishes after the peers reconnect, files identical", wait(lambda: items().get("Big8M", {}).get("status") == "complete", 90) and same("Big8M/big.bin") and same("Big8M/extra/more.bin"))
check("no 'stopping cleanly' after a single event", "stopping cleanly" not in log())
quit_()
boot("daemon_mem", ["Big8M.torrent"], 0.05); time.sleep(4)
for k in range(5):
    open(FT + "/force_oom", "w").write("1"); time.sleep(1.2)
check("five out-of-memory events within 10 minutes: the program stops cleanly (it cannot work like that)", wait(lambda: "out of memory again and again -> stopping cleanly" in log(), 6))
time.sleep(5)
try: st(); alive = True
except Exception: alive = False
check("and it really exits", not alive)
print("RESULT: %d/%d passed" % (sum(res), len(res)))
