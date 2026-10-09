import urllib.request, json, time, os, subprocess, signal
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def status(t=3): return json.load(urllib.request.urlopen(B + "/status", timeout=t))
def log(): return open(FT + "/log/log.txt").read()
def alive(p): return p.poll() is None
def start(tag):
    return subprocess.Popen([FT + "/daemon_net2"], stdout=open(FT + "/%s.out" % tag, "w"), stderr=subprocess.STDOUT, cwd=FT, preexec_fn=os.setsid)
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.1)
    return False
def kill(p):
    try: os.killpg(p.pid, signal.SIGKILL)
    except Exception: pass

print("== E. a new copy replaces the running one (this is what sending a new ELF does)")
A = start("copyA"); check("copy A is serving", wait(lambda: status()["version"] != "", 8))
t0 = time.time(); Bp = start("copyB")
check("copy A quit by itself (asked through the flag file)", wait(lambda: not alive(A), 10))
check("copy B took over and serves (%.1f s after its start)" % (time.time() - t0), wait(lambda: status()["version"] != "", 10))
dt = time.time() - t0
check("the whole replacement took less than 6 s: %.1f s" % dt, dt < 6)
l = log()
check("log of the new copy keeps the note about the replacement", "the previous copy was asked to quit and replaced" in l)
check("only one copy is left running", alive(Bp) and not alive(A))
check("no stale flag file is left", not os.path.exists(FT + "/log/quit.flag"))

print("== F. the old copy does not react (hung): the new copy must give up and NOT disturb it")
open(FT + "/ignore_quit_flag", "w").write("1")
t0 = time.time(); C = start("copyC")
check("copy C gives up after the wait (exit code %s)" % (C.wait(timeout=20) if wait(lambda: not alive(C), 15) else "still running"), wait(lambda: not alive(C), 15) and C.returncode == 1)
check("it gave up after about 8 s (%.1f s)" % (time.time() - t0), 7 <= time.time() - t0 <= 12)
check("the working copy keeps serving", status()["version"] != "")
check("copy C removed the flag it had created", not os.path.exists(FT + "/log/quit.flag"))
check("log says it gave up", "the previous copy did not stop within 8 s" in log())
os.remove(FT + "/ignore_quit_flag")

print("== G. the old copy is frozen for a few seconds (stuck drive) when the new copy starts")
open(FT + "/stall_main", "w").write("4")
time.sleep(0.8)
t0 = time.time(); Dp = start("copyD")
check("the frozen copy quits as soon as it wakes up, copy D takes over", wait(lambda: status()["version"] != "" and not alive(Bp), 15))
check("it took %.1f s (longer than E because of the freeze, but within the wait limit)" % (time.time() - t0), time.time() - t0 < 10)
check("copy D is the one running", alive(Dp))
kill(Dp)
print("RESULT: %d/%d passed" % (sum(res), len(res)))
