# Завершённая раздача со снятым файлом: возвращаем галочку -> раздача снова качается, докачивает только недостающее.
# Заодно проверяет защиту от сна (счётчик вызовов /tmp/ft/keep_awake_calls растёт, пока идёт загрузка).
import urllib.request, urllib.error, json, time, os, shutil, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; U = FT + "/usb0/torrents"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def get(p): return json.load(urllib.request.urlopen(B + p, timeout=15))
def add(name, extra):
    data = open(FT + "/torrents_src/" + name, "rb").read()
    r = urllib.request.Request(B + "/api/add?drive=0&name=" + urllib.request.quote(name) + extra, data=data, method="POST")
    return json.load(urllib.request.urlopen(r, timeout=15))
def items(): return {i["title"]: i for i in get("/status")["items"]}
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.3)
    return False
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def boot(delay):
    sh("pkill -9 -x daemon_sel; bash seeder_ctl.sh stop; rm -rf usb0 usb1 internal log keep_awake_calls; mkdir -p usb0/torrents log; bash seeder_ctl.sh start %s 0 0 0 0" % delay)
    subprocess.Popen("setsid ./daemon_sel >> reopen.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)
def quit_():
    try: urllib.request.urlopen(B + "/quit", timeout=3).read()
    except Exception: pass
    time.sleep(3.5)
def same(rel): return os.path.exists(U + "/downloads/" + rel) and open(FT + "/src/" + rel, "rb").read() == open(U + "/downloads/" + rel, "rb").read()
def calls():
    try: return int(open(FT + "/keep_awake_calls").read())
    except Exception: return 0

print("=== ROUND 1: Big8M with the second file deselected, then the file is added after completion")
boot(0)
j = add("Big8M.torrent", "&start=1&skip=1")
check("accepted", j.get("ok"))
check("completes without the second file", wait(lambda: items().get("Big8M", {}).get("status") == "complete", 60))
h = items()["Big8M"]["hash"]
check("sleep guard was poked while downloading (%d calls)" % calls(), calls() >= 1)
check(".torrent is in complete/", os.path.exists(U + "/complete/Big8M.torrent") and not os.path.exists(U + "/Big8M.torrent"))
f = get("/api/files?hash=" + h)
check("file list of a finished task: ok, complete=%s, 2 files, sel [True, False]" % f.get("complete"),
      f.get("ok") and f.get("complete") and len(f["files"]) == 2 and [x["sel"] for x in f["files"]] == [True, False])
check("extra file absent on disk", not os.path.exists(U + "/downloads/Big8M/extra/more.bin"))
urllib.request.urlopen(B + "/api/select?hash=" + h + "&skip=", timeout=10).read()
check("the task is active again (not complete)", wait(lambda: items().get("Big8M", {}).get("status") in ("downloading", "checking", "queued"), 15))
check("the task completes again", wait(lambda: items().get("Big8M", {}).get("status") == "complete", 90))
it = items()["Big8M"]
check("nskip=0 and 100%%: nskip=%d pct=%d" % (it["nskip"], it["pct"]), it["nskip"] == 0 and it["pct"] == 100)
check("both files identical to the source", same("Big8M/big.bin") and same("Big8M/extra/more.bin"))
check(".torrent is back in complete/", wait(lambda: os.path.exists(U + "/complete/Big8M.torrent") and not os.path.exists(U + "/Big8M.torrent"), 10))
quit_()
print("=== ROUND 2: same, but restart right after the file is added (state survives)")
boot(0.002)
add("Big8M.torrent", "&start=1&skip=1")
wait(lambda: items().get("Big8M", {}).get("status") == "complete", 60)
h = items()["Big8M"]["hash"]
urllib.request.urlopen(B + "/api/select?hash=" + h + "&skip=", timeout=10).read()
time.sleep(1.5)
quit_()
sh("rm -f keep_awake_calls")
subprocess.Popen("setsid ./daemon_sel >> reopen.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)
check("after restart the task is listed and not marked complete with a missing file",
      wait(lambda: "Big8M" in items(), 15))
st = items()["Big8M"]["status"]
check("after restart the task continues or completes (status %s)" % st, st != "complete" or same("Big8M/extra/more.bin"))
check("the task completes with both files identical", wait(lambda: items().get("Big8M", {}).get("status") == "complete" and same("Big8M/extra/more.bin"), 120) and same("Big8M/big.bin"))
quit_()
print("=== ROUND 3: nothing downloading -> no sleep-guard calls")
boot(0)
time.sleep(12)
check("no calls with an empty list", calls() == 0)
quit_()
sh("pkill -9 -x daemon_sel; bash seeder_ctl.sh stop")
print("RESULT %d/%d" % (sum(res), len(res)))
