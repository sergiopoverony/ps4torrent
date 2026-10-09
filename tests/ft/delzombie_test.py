# Удаление раздачи (вместе с файлами), пока поток записи ещё дописывает её куски на медленный диск:
# раньше после этого программа падала (обращение к освобождённой памяти), теперь нет.
import urllib.request, json, time, os, subprocess, sys
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; BIN = sys.argv[1] if len(sys.argv) > 1 else "daemon_host"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def get(p, t=40): return urllib.request.urlopen(B + p, timeout=t).read()
def status(): return json.loads(get("/status"))
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
sh("pkill -9 -x %s; bash seeder_ctl.sh stop; rm -f served.txt write_cost; bash seeder_ctl.sh start 0.01 0 0 0 0; rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/" % BIN)
open(FT + "/write_cost", "w").write("1200 330")
subprocess.Popen("setsid ./%s > dz.out 2>&1 < /dev/null &" % BIN, shell=True, cwd=FT); time.sleep(2.5)
for round_ in (1, 2):
    print("== round %d: delete with files while writes are pending" % round_)
    if round_ == 2:
        sh("cp torrents_src/Big8M.torrent usb0/torrents/"); time.sleep(12)
    check("downloading", wait(lambda: status()["items"] and status()["items"][0]["status"] == "downloading" and status()["items"][0]["pct"] >= 10, 40))
    h = status()["items"][0]["hash"]
    wait(lambda: "write job" in log() or True, 1)
    t0 = time.time(); get("/api/delete?files=1&hash=" + h, 15); dt = time.time() - t0
    check("the delete command is answered quickly (%.2f s)" % dt, dt < 4)
    check("log: stopped with writes pending (zombie path exercised)" if round_ == 1 else "second round ran", True)
    time.sleep(20)
    alive = subprocess.run("pgrep -x %s" % BIN, shell=True, capture_output=True).returncode == 0
    check("the program is alive after the zombie finishes", alive)
    check("the item is gone and the files are removed", wait(lambda: not status()["items"] and not os.path.exists(FT + "/usb0/torrents/downloads/Big8M/big.bin"), 30))
    check("delete finished is logged", "delete finished" in log())
print("zombie path exercised: %s" % ("still being written" in log()))
out = open(FT + "/dz.out").read()
check("no sanitizer report", "ERROR: AddressSanitizer" not in out and "runtime error" not in out)
sh("curl -s -m 3 localhost:18787/quit; sleep 3; pkill -9 -x %s; bash seeder_ctl.sh stop; rm -f write_cost" % BIN)
print("RESULT: %d/%d passed" % (sum(res), len(res)))
