import urllib.request, json, time, os, subprocess, shutil
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; H = "6b5d373445b803f9bb3630e88ae74f1617438cc2"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def get(p, t=15): return urllib.request.urlopen(B + p, timeout=t).read()
def status(): return json.loads(get("/status"))
def log(): return open(FT + "/log/log.txt").read()
def checks(): return [l for l in log().split("\n") if l.startswith("storage check #")]
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.4)
    return False

check("startup log records who the process is: %s" % [l for l in log().split("\n") if l.startswith("process:")][:1], any(l.startswith("process: uid=") for l in log().split("\n")))

print("== the drive disappears (mount point gone)")
shutil.move(FT + "/usb0", FT + "/usb0_gone")
check("offline detected", wait(lambda: "drive offline" in log(), 20))
check("a 'storage check' line appears within ~25 s", wait(lambda: len(checks()) >= 1, 30))
c = checks()[0] if checks() else ""
print("   ", c[:420])
check("it says the mount point is ABSENT with the errno", "usb0: ABSENT (errno 2" in c)
check("it says the torrents folder is ABSENT", "torrents folder ABSENT" in c)
check("it lists what is mounted and which device nodes exist", "[usb0" in c and "devices: [da0 da0s1" in c)
check("it records the process identity (uid/euid/gid/jailed)", "process: uid=" in c and "jailed=" in c)

print("== mount point present but WITHOUT the torrents folder (what a wrongly mounted drive looks like)")
os.makedirs(FT + "/usb0", exist_ok=True); open(FT + "/usb0/other_file.txt", "w").write("x")
n = len(checks())
check("the next check reports PRESENT + torrents folder ABSENT + the entries it sees", wait(lambda: len(checks()) > n and "usb0: PRESENT" in checks()[-1] and "torrents folder ABSENT" in checks()[-1] and "other_file.txt" in checks()[-1], 60))
print("   ", (checks() or [""])[-1][:300])

print("== the drive returns: tasks come back, the checks stop")
shutil.rmtree(FT + "/usb0"); shutil.move(FT + "/usb0_gone", FT + "/usb0")
check("drive back online", wait(lambda: "drive back online" in log(), 25))
n = len(checks()); time.sleep(45)
check("no more storage checks once nothing is offline", len(checks()) == n)

print("== a file-system call HANGS on the dead mount (simulated): the program must stay responsive")
open(FT + "/probe_hang", "w").write("1")
shutil.move(FT + "/usb0", FT + "/usb0_gone")
lat = []
t0 = time.time()
while time.time() - t0 < 38:
    t = time.time(); get("/api/set?max_parallel=3"); lat.append(time.time() - t); time.sleep(1)
check("the program stayed responsive while the probe hung (worst command latency %.2f s)" % max(lat), max(lat) < 0.5)
check("the log says the call is HUNG on that path", wait(lambda: any("NO ANSWER" in l for l in checks()), 15))
os.remove(FT + "/probe_hang"); shutil.move(FT + "/usb0_gone", FT + "/usb0")
print("RESULT: %d/%d passed" % (sum(res), len(res)))
