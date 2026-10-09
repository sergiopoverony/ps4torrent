import urllib.request, json, time, os, subprocess
FT="/tmp/ft"; B="http://127.0.0.1:18787"; H="6b5d373445b803f9bb3630e88ae74f1617438cc2"
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
sh("pkill -9 -x daemon_host; bash seeder_ctl.sh stop; rm -f served.txt write_cost; bash seeder_ctl.sh start 0.01 0 0 0 0; rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/")
open(FT+"/write_cost","w").write("12000 0")                     # 3 с на вызов (вместо 30): ускоряем тест
subprocess.Popen("setsid ./daemon_host > wr_dbg2.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)
time.sleep(7); urllib.request.urlopen(B+"/api/pause?hash="+H, timeout=40).read()
for i in range(24):
    time.sleep(2)
    t=time.time(); urllib.request.urlopen(B+"/api/set?max_parallel=3", timeout=40).read(); lat=time.time()-t
    st = json.load(urllib.request.urlopen(B+"/status", timeout=5))["items"][0]
    print("t+%2ds cmd-latency %.2fs status=%s done=%d MB | log has saved-line: %s" % (i*2+2, lat, st["status"], st["done"]>>20, "progress saved" in open(FT+"/log/log.txt").read()), flush=True)
print("---- full log tail:")
print(open(FT+"/log/log.txt").read()[-1400:])
print("daemon alive:", subprocess.run("pgrep -x daemon_host", shell=True, capture_output=True).returncode == 0)
sh("curl -s -m 3 localhost:18787/quit; sleep 3; pkill -9 -x daemon_host; rm -f write_cost; bash seeder_ctl.sh stop")
