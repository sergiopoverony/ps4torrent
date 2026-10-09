#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_new 2>/dev/null
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0 0 0 2        # данные пачками: 640 КБ раз в 2 с (~320 КБ/с)
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Many.torrent usb0/torrents/
setsid ./daemon_new > eta2.out 2>&1 < /dev/null &
python3 - << 'PY' > eta2.result
import urllib.request, json, time
H = "4a02b3f8a2bee94a9da132ff46e2445fe434290a"
def st():
    d = json.load(urllib.request.urlopen("http://127.0.0.1:18787/status", timeout=2)); return d["items"][0]
t0 = time.time(); rows = []
while time.time() - t0 < 60:
    try: i = st(); rows.append((time.time()-t0, i["status"], i["pct"], i["speed_kb"], i["eta_s"]))
    except Exception: pass
    time.sleep(3)
print("BURSTY DATA (640 KB every 2 s): ETA must go down smoothly")
for r in rows: print("  %4.0fs %-11s %3d%% speed %4d  ETA %5d" % r)
etas = [r[4] for r in rows if r[4] >= 0]
print("  ETA jumps between consecutive samples (s):", [etas[k+1]-etas[k] for k in range(len(etas)-1)][:12])
urllib.request.urlopen("http://127.0.0.1:18787/api/pause?hash=" + H, timeout=2).read(); time.sleep(3)
i = st(); print("AFTER PAUSE: status=%s speed=%d eta=%d (expect paused / 0 / -1)" % (i["status"], i["speed_kb"], i["eta_s"]))
urllib.request.urlopen("http://127.0.0.1:18787/api/resume?hash=" + H, timeout=2).read(); time.sleep(4)
i = st(); print("4 s AFTER RESUME: status=%s eta=%d (expect unknown = -1 during warm-up)" % (i["status"], i["eta_s"]))
time.sleep(14)
i = st(); print("18 s AFTER RESUME: status=%s eta=%d (expect a number again)" % (i["status"], i["eta_s"]))
PY
timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 2; pkill -9 -x daemon_new 2>/dev/null
echo done >> eta2.result
