# Повторное добавление того же торрента: сообщается, второй копии не создаётся; завершённый можно добавить заново.
import urllib.request, urllib.error, json, time, os, subprocess, sys
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; BIN = sys.argv[1]
res = []
def check(name, ok): ok = bool(ok); res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def get(p, t=20): return urllib.request.urlopen(B + p, timeout=t).read()
def items(): return {i["title"]: i for i in json.loads(get("/status"))["items"]}
def add(name, extra=""):
    data = open(FT + "/torrents_src/" + name, "rb").read()
    r = urllib.request.Request(B + "/api/add?name=" + urllib.request.quote(name) + extra, data=data, method="POST")
    return json.load(urllib.request.urlopen(r, timeout=20))
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.3)
    return False
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def torrents(): return sorted(f for f in os.listdir(FT + "/usb0/torrents") if f.endswith(".torrent"))
sh("pkill -9 -x %s; bash seeder_ctl.sh stop; rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log; bash seeder_ctl.sh start 0 0 0 0 0" % BIN)
subprocess.Popen("setsid ./%s > dup.out 2>&1 < /dev/null &" % BIN, shell=True, cwd=FT); time.sleep(2.5)
print("== an unfinished torrent")
r1 = add("Game One.torrent", "&drive=0&start=0")
check("first add: ok, not a duplicate", r1.get("ok") and not r1.get("duplicate"))
r2 = add("Game One.torrent", "&drive=0&start=0")     # сразу, до обновления списка
check("immediate second add is reported as a duplicate (%s)" % r2, r2.get("ok") and r2.get("duplicate") is True and r2.get("title"))
check("the item is in the list once", wait(lambda: "Game One" in items(), 15))
r3 = add("Game One.torrent", "&drive=0&start=0")
check("third add (after the list updated) is also a duplicate with the real title: %s" % r3.get("title"), r3.get("duplicate") is True and r3.get("title") == "Game One")
check("only one .torrent file exists, no '(2)' copy: %s" % torrents(), torrents() == ["Game One.torrent"] and len(items()) == 1)
r4 = add("Small.torrent", "&drive=0&start=1")
check("another torrent is not affected", r4.get("ok") and not r4.get("duplicate"))
print("== a finished torrent can be added again")
check("Small completes", wait(lambda: items().get("Small", {}).get("status") == "complete", 40))
r5 = add("Small.torrent", "&drive=0&start=1")
check("re-adding a FINISHED torrent is allowed (starts over)", r5.get("ok") and not r5.get("duplicate"))
out = open(FT + "/dup.out").read()
check("no sanitizer report", "ERROR: AddressSanitizer" not in out and "runtime error" not in out)
sh("curl -s -m 3 localhost:18787/quit; sleep 3; pkill -9 -x %s; bash seeder_ctl.sh stop" % BIN)
print("RESULT: %d/%d passed" % (sum(res), len(res)))
