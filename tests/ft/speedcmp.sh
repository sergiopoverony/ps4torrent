#!/bin/bash
# speedcmp.sh <binary> <label>: 25 замеров speed_kb со страницы раз в секунду при данных пачками
BIN=$1; LABEL=$2
cd /tmp/ft; pkill -9 -x $(basename $BIN) 2>/dev/null
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0 0 0 2
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/
setsid $BIN > speed_$LABEL.out 2>&1 < /dev/null &
sleep 8
python3 - << PY > speed_$LABEL.result
import urllib.request, json, time
vals = []
for i in range(25):
    try:
        d = json.load(urllib.request.urlopen("http://127.0.0.1:18787/status", timeout=2)); vals.append(d["speed_kb"])
    except Exception as e: vals.append(-1)
    time.sleep(1)
print("$LABEL", "samples:", vals)
nz = [v for v in vals if v >= 0]
print("$LABEL", "min", min(nz), "max", max(nz), "zeros", sum(1 for v in nz if v == 0), "of", len(nz))
PY
timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 2; pkill -9 -x $(basename $BIN) 2>/dev/null
echo done >> speed_$LABEL.result
