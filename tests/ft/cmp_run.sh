#!/bin/bash
# cmp_run.sh <binary> <label>: тест "сид замолкает на 100 с посреди первого куска"
BIN=$1; LABEL=$2
cd /tmp/ft
pkill -9 -x $(basename $BIN) 2>/dev/null
bash seeder_ctl.sh stop; rm -f served.txt
bash seeder_ctl.sh start 0.002 300 100
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/
T0=$(date +%s)
setsid $BIN > cmp_$LABEL.out 2>&1 < /dev/null &
for i in $(seq 1 270); do
  st=$(timeout 3 curl -s -m 2 localhost:18787/status | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['items'][0]['status'] if d['items'] else 'none')" 2>/dev/null)
  [ "$st" = "complete" ] && break
  sleep 1
done
T1=$(date +%s)
sleep 2
{
echo "LABEL=$LABEL status=$st seconds=$((T1-T0)) served_blocks=$(cat served.txt 2>/dev/null)"
grep -E "stalled|got nothing|stuck|closed \(|no data for" log/log.txt | head -12
cmp -s src/Big8M/big.bin usb0/torrents/downloads/Big8M/big.bin && echo "big.bin IDENTICAL" || echo "big.bin DIFFERENT"
cmp -s src/Big8M/extra/more.bin usb0/torrents/downloads/Big8M/extra/more.bin && echo "more.bin IDENTICAL" || echo "more.bin DIFFERENT"
} > cmp_$LABEL.result 2>&1
timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 2; pkill -9 -x $(basename $BIN) 2>/dev/null
echo done >> cmp_$LABEL.result
