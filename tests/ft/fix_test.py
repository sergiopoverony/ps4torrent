import urllib.request, json, time, os, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; R = FT + "/usb0/torrents"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def st1(): return json.load(urllib.request.urlopen(B + "/status", timeout=8))["items"][0]
def log(): return open(FT + "/log/log.txt").read()
def served():
    try: return int(open(FT + "/served.txt").read())
    except Exception: return 0
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.3)
    return False
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
sh("pkill -9 -x daemon_fix; bash seeder_ctl.sh stop; rm -f served.txt write_cost storage_fail stall_main; bash seeder_ctl.sh start 0.005 0 0 0 0; rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/")
open(FT + "/write_cost", "w").write("9000 0")                 # каждая запись на диск занимает 9 секунд
open(FT + "/storage_fail", "w").write("2")                    # первая запись (оба захода) не удаётся: кусок 0 пропадёт
subprocess.Popen("setsid ./daemon_fix > fix.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)

print("== the first queued piece fails, the piece behind it is written")
check("piece 0 WRITE FAILED is logged", wait(lambda: "piece 0 WRITE FAILED" in log(), 40))
check("the errno line hints at a disconnected drive", "[the drive may have been disconnected]" in log())
check("the task stops ('write failed') and waits for the retry (60 s in this build)", wait(lambda: "will retry in 60 s" in log(), 10))
check("the stopped task reports the writes still running (job count)", "write job(s) of Big8M are still being written to the drive" in log())
check("when the writes end the log tells the TRUTH about the lost piece", wait(lambda: "WARNING: 1 verified piece(s) of Big8M could NOT be written" in log(), 80))
check("it does NOT claim that all writes succeeded", "disk writes of Big8M finished, progress saved" not in log())
os.remove(FT + "/write_cost"); os.remove(FT + "/storage_fail")

print("== after a pause (console sleep) the failed task restarts at once instead of waiting 60 s")
t0 = time.time(); open(FT + "/stall_main", "w").write("5")
check("pause detected, log shows monotonic AND wall-clock gaps", wait(lambda: "pause: the program was not running for" in log() and "monotonic" in log() and "wall clock" in log(), 12))
print("   ", [x for x in log().split("\n") if x.startswith("pause:")][:1])
check("the failed task restarts within ~12 s of the pause (not 60 s)", wait(lambda: log().count("torrent: Big8M") >= 2, 14) and time.time() - t0 < 25)
check("it resumes with the piece that WAS written (1 of 5 pieces already verified)", wait(lambda: "(1 of 5 pieces already verified)" in log() or "already have 1 of 5 pieces" in log(), 8))
check("the task completes", wait(lambda: st1()["status"] == "complete", 40))
sv = served()
check("only the lost piece was downloaded again (blocks served %d; ~2137 + 512 = ~2650; losing the written piece too would give ~3150)" % sv, sv < 2900)
ok = all(open(FT + "/src/Big8M/" + f, "rb").read() == open(R + "/downloads/Big8M/" + f, "rb").read() for f in ("big.bin", "extra/more.bin"))
check("files identical", ok)
print("RESULT: %d/%d passed" % (sum(res), len(res)))
