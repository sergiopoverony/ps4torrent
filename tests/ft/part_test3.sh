#!/bin/bash
cd /tmp/ft
H=6b5d373445b803f9bb3630e88ae74f1617438cc2
pkill -9 -x daemon_host 2>/dev/null; rm -rf usb0 usb1 log served.txt; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0.002 300 0 0
setsid ./daemon_host > b1.out 2>&1 < /dev/null &
sleep 13; timeout 3 curl -s -m 2 "localhost:18787/api/pause?hash=$H" >/dev/null; sleep 3; timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 3
python3 -c "
p='/tmp/ft/usb0/torrents/downloads/Big8M/big.bin'; b=bytearray(open(p,'rb').read())
for k in range(1000000,1000040): b[k]^=0xFF
open(p,'wb').write(b); print('corrupted 40 bytes inside a persisted block')"
bash seeder_ctl.sh stop; rm -f served.txt; bash seeder_ctl.sh start 0 0 0 0
T0=$(date +%s); setsid ./daemon_host > b2.out 2>&1 < /dev/null &
for i in $(seq 1 90); do st=$(timeout 3 curl -s -m 2 localhost:18787/status | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['items'][0]['status'])" 2>/dev/null); [ "$st" = "complete" ] && break; sleep 1; done
echo "status: $st after $(( $(date +%s) - T0 )) s; blocks served: $(cat served.txt 2>/dev/null) (expect ~1837 + 512 re-download of piece 0 = ~2349)"
grep -E "restored|verification|MISMATCH|dropping" log/log.txt | head -4
for f in "Big8M/big.bin" "Big8M/extra/more.bin"; do cmp -s "src/$f" "usb0/torrents/downloads/$f" && echo "IDENTICAL $f" || echo "DIFFERENT $f"; done
timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 3; echo "sanitizer: [$(grep -v notify b2.out | head -3)]"
