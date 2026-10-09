#!/bin/bash
cd /tmp/ft
H=6b5d373445b803f9bb3630e88ae74f1617438cc2
pkill -9 -x daemon_host 2>/dev/null
rm -rf usb0 usb1 log served.txt; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/
{
echo "== phase 1: seeder goes silent after 300 blocks, then PAUSE"
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0.002 300 0 0
setsid ./daemon_host > p1.out 2>&1 < /dev/null &
sleep 14
timeout 3 curl -s -m 2 "localhost:18787/api/pause?hash=$H"; echo; sleep 3
echo "-- .part file after pause:"; cat usb0/torrents/.state/$H.part 2>/dev/null | cut -c1-100 || echo "(none)"
echo "-- file sizes on flash (partial blocks were written into the final files):"; ls -la usb0/torrents/downloads/Big8M/ 2>/dev/null | head -5
timeout 3 curl -s -m 2 "localhost:18787/api/resume?hash=$H" >/dev/null; sleep 6
echo "-- after RESUME in the same process:"; grep -E "restored|partial|stalled" log/log.txt | tail -3
timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 3
echo "-- sanitizer output phase 1 (empty = clean):"; grep -v notify p1.out | head -5
echo
echo "== phase 2: restart daemon with a normal seeder"
bash seeder_ctl.sh stop; rm -f served.txt; bash seeder_ctl.sh start 0 0 0 0
setsid ./daemon_host > p2.out 2>&1 < /dev/null &
for i in $(seq 1 60); do st=$(timeout 3 curl -s -m 2 localhost:18787/status | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['items'][0]['status'])" 2>/dev/null); [ "$st" = "complete" ] && break; sleep 1; done
sleep 2
echo "status: $st"; grep -E "restored" log/log.txt | tail -2
echo "blocks served in phase 2: $(cat served.txt 2>/dev/null)  (a full download is ~2137, minus the ~300 already stored)"
for f in "Big8M/big.bin" "Big8M/extra/more.bin"; do cmp -s "src/$f" "usb0/torrents/downloads/$f" && echo "IDENTICAL $f" || echo "DIFFERENT $f"; done
echo "-- .part file after completion (must be gone):"; ls usb0/torrents/.state/ 2>/dev/null
timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 3
echo "-- sanitizer output phase 2 (empty = clean):"; grep -v notify p2.out | head -5
} > part_test.result 2>&1
echo done >> part_test.result
