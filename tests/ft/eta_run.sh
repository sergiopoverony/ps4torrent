#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_new 2>/dev/null
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0.04 0 0 0
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Many.torrent usb0/torrents/
setsid ./daemon_new > eta.out 2>&1 < /dev/null &
python3 - << 'PY' > eta.result
import urllib.request, json, time
t0 = time.time(); rows = []
done_at = None
while time.time() - t0 < 270:
    try:
        d = json.load(urllib.request.urlopen("http://127.0.0.1:18787/status", timeout=2))
        if d["items"]:
            i = d["items"][0]; rows.append((time.time() - t0, i["status"], i["pct"], i["speed_kb"], i["eta_s"]))
            if i["status"] == "complete": done_at = time.time() - t0; break
    except Exception as e: pass
    time.sleep(5)
print("completed at %.0f s" % (done_at or -1))
print("  t(s)  status       pct  speed  ETA(s)  actual_left(s)  ETA/actual")
for t, st, pct, sp, eta in rows:
    if done_at and st == "downloading":
        act = done_at - t
        ratio = ("%.2f" % (eta / act)) if eta >= 0 and act > 1 else "-"
        print("%6.0f  %-11s %3d%%  %5d  %6d  %12.0f   %s" % (t, st, pct, sp, eta, act, ratio))
    else:
        print("%6.0f  %-11s %3d%%  %5d  %6d" % (t, st, pct, sp, eta))
PY
timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 2; pkill -9 -x daemon_new 2>/dev/null
echo done >> eta.result
