import urllib.request, json, time, os, shutil, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; U = FT + "/usb0/torrents"
def st(): return json.load(urllib.request.urlopen(B + "/status", timeout=8))
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
sh("pkill -9 -x daemon_mem; bash seeder_ctl.sh stop; rm -rf usb0 usb1 internal log force_oom; mkdir -p usb0/torrents log; bash seeder_ctl.sh start 0.02 0 0 0 0")
shutil.copy(FT + "/torrents_src/Big8M.torrent", U + "/Big8M.torrent")
subprocess.Popen("setsid ./daemon_mem > mem.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)
t0 = time.time()
while time.time() - t0 < 30:
    i = st()["items"][0]
    if i["pct"] >= 15: break
    time.sleep(0.3)
print("before: %d%% %s" % (i["pct"], i["status"]))
open(FT + "/force_oom", "w").write("1")
for k in range(14):
    time.sleep(5); i = st()["items"][0]; print("t+%2ds: %3d%% %-12s %4d KB/s" % ((k + 1) * 5, i["pct"], i["status"], i["speed_kb"]), flush=True)
    if i["status"] == "complete": break
L = open(FT + "/log/log.txt").read().split("\n")
n = [x for x, l in enumerate(L) if "OUT OF MEMORY" in l][0]
print("--- log after the event:")
for l in L[n:n + 28]: print("  ", l[:150])
sh("curl -s -m 3 localhost:18787/quit; sleep 3; pkill -9 -x daemon_mem; bash seeder_ctl.sh stop")
