import urllib.request, json, time, os, subprocess
FT="/tmp/ft"; B="http://127.0.0.1:18787"; H="6b5d373445b803f9bb3630e88ae74f1617438cc2"
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
sh("pkill -9 -x daemon_host; bash seeder_ctl.sh stop; rm -f served.txt write_cost; bash seeder_ctl.sh start 0.01 0 0 0 0; rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/")
open(FT+"/write_cost","w").write("30000 0")
subprocess.Popen("setsid ./daemon_host > wr_dbg.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)
time.sleep(7); t0=time.time(); urllib.request.urlopen(B+"/api/pause?hash="+H, timeout=40).read(); print("pause %.2f s" % (time.time()-t0), flush=True)
for i in range(28):
    time.sleep(3)
    try:
        st = json.load(urllib.request.urlopen(B+"/status", timeout=5))["items"][0]
        print("t+%2ds status=%s done=%d MB" % (i*3+3, st["status"], st["done"]>>20), flush=True)
    except Exception as e: print("status error", e, flush=True)
print("---- relevant log lines:")
for l in open(FT+"/log/log.txt").read().split("\n"):
    if any(k in l for k in ("written to the drive","progress saved","WRITE","storage:","slow main","pause:","stopped:")): print("  ", l[:170])
sh("curl -s -m 3 localhost:18787/quit; sleep 3; pkill -9 -x daemon_host; rm -f write_cost; bash seeder_ctl.sh stop")
