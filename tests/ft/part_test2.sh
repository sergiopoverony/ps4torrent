#!/bin/bash
cd /tmp/ft
H=6b5d373445b803f9bb3630e88ae74f1617438cc2
start_fresh() { pkill -9 -x daemon_host 2>/dev/null; rm -rf usb0 usb1 log served.txt; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/; bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0.002 300 0 0; }
finish_normal() { bash seeder_ctl.sh stop; rm -f served.txt; bash seeder_ctl.sh start 0 0 0 0
  setsid ./daemon_host > $1 2>&1 < /dev/null &
  for i in $(seq 1 60); do st=$(timeout 3 curl -s -m 2 localhost:18787/status | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['items'][0]['status'])" 2>/dev/null); [ "$st" = "complete" ] && break; sleep 1; done; sleep 1
  echo "status: $st; blocks served: $(cat served.txt 2>/dev/null)"; grep -E "restored|failed hash|hash" log/log.txt | head -3
  for f in "Big8M/big.bin" "Big8M/extra/more.bin"; do cmp -s "src/$f" "usb0/torrents/downloads/$f" && echo "IDENTICAL $f" || echo "DIFFERENT $f"; done
  timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 3; echo "sanitizer: [$(grep -v notify $1 | head -3)]"; }
{
echo "===== A: process killed with kill -9 (no clean shutdown) after the 20 s periodic save"
start_fresh; setsid ./daemon_host > a1.out 2>&1 < /dev/null &
sleep 27; pkill -9 -x daemon_host; sleep 1
echo ".part after kill -9: $(ls usb0/torrents/.state/ 2>/dev/null | tr '\n' ' ')"
finish_normal a2.out
echo
echo "===== B: persisted block data corrupted on disk, then restart"
start_fresh; setsid ./daemon_host > b1.out 2>&1 < /dev/null &
sleep 13; timeout 3 curl -s -m 2 "localhost:18787/api/pause?hash=$H" >/dev/null; sleep 3; timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 3
python3 - << 'PY'
p = "/tmp/ft/usb0/torrents/downloads/Big8M/big.bin"
b = bytearray(open(p, "rb").read()); 
for k in range(1000000, 1000040): b[k] ^= 0xFF          # портим 40 байт внутри уже сохранённого блока
open(p, "wb").write(b); print("corrupted 40 bytes inside a persisted block")
PY
finish_normal b2.out
echo
echo "===== C: .part file is garbage"
start_fresh; setsid ./daemon_host > c1.out 2>&1 < /dev/null &
sleep 13; timeout 3 curl -s -m 2 "localhost:18787/api/pause?hash=$H" >/dev/null; sleep 3; timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 3
head -c 300 /dev/urandom > usb0/torrents/.state/$H.part; echo "replaced .part with random bytes"
finish_normal c2.out
} > part_test2.result 2>&1
echo done >> part_test2.result
