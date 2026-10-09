import urllib.request, json, time, os, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; R = FT + "/usb0/torrents"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def status(t=5): return json.load(urllib.request.urlopen(B + "/status", timeout=t))
def st1(): return status()["items"][0]
def log(): return open(FT + "/log/log.txt").read()
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.2)
    return False
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def identical():
    return all(open(FT + "/src/Big8M/" + f, "rb").read() == open(R + "/downloads/Big8M/" + f, "rb").read() for f in ("big.bin", "extra/more.bin"))
def fresh(seeder):
    sh("pkill -9 -x daemon_net2; bash seeder_ctl.sh stop; rm -f storage_fail stall_main netmock write_cost; bash seeder_ctl.sh start %s; rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/" % seeder)
def start():
    subprocess.Popen("setsid ./daemon_net2 > fd.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)

print("== 1. one write fails (stale handle after the drive was re-attached): the writer thread retries with a fresh handle")
fresh("0 0 0 0 0"); open(FT + "/storage_fail", "w").write("1"); start()
check("download completes", wait(lambda: st1()["status"] == "complete", 25))
l = log()
check("log shows errno and the reopen", "storage: write failed on" in l and "(errno 5) [the drive may have been disconnected], reopening the file and trying again" in l)
check("no 'WRITE FAILED'", "WRITE FAILED" not in l)
check("files identical", identical())
sh("curl -s -m 3 localhost:18787/quit; sleep 3")

print("== 2. writes keep failing for a while, then work again")
fresh("0 0 0 0 0"); open(FT + "/storage_fail", "w").write("100000"); start()
check("task stops with 'write failed' and retries quickly (4 s in this build)", wait(lambda: "will retry in 4 s" in log(), 25))
check("log has 'still failing'", "still failing" in log())
os.remove(FT + "/storage_fail")
check("after the drive is back the task finishes by itself", wait(lambda: st1()["status"] == "complete", 40))
check("files identical", identical())
sh("curl -s -m 3 localhost:18787/quit; sleep 3")

print("== 3. the program was frozen (console sleep): open files are closed, peers are allowed again")
fresh("0.01 0 0 0 0"); start()
check("first piece is written (files are open)", wait(lambda: st1()["done"] > 0, 25))
open(FT + "/stall_main", "w").write("5")
check("log: pause detected", wait(lambda: "pause: the program was not running for" in log(), 12))
check("log: recovery line", wait(lambda: any(x.startswith("recovery:") for x in log().split("\n")), 6))
print("   ", [x for x in log().split("\n") if x.startswith("recovery:")][:1])
check("the download completes with identical files", wait(lambda: st1()["status"] == "complete", 90) and identical())
sh("curl -s -m 3 localhost:18787/quit; sleep 3")

print("== 4. the network comes back")
fresh("0.02 0 0 0 0"); open(FT + "/netmock", "w").write(""); start(); time.sleep(3)
open(FT + "/netmock", "w").write("192.168.0.50")
check("log: recovery on network return", wait(lambda: "recovery: 1 downloads will reconnect to peers" in log(), 10))
sh("curl -s -m 3 localhost:18787/quit; sleep 3; rm -f netmock")
print("RESULT: %d/%d passed" % (sum(res), len(res)))
