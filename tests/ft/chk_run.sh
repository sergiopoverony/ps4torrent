#!/bin/bash
# chk_run.sh <binary>: полная проверка 400 МБ уже лежащих на флешке данных; замеряем время и загрузку процессора
BIN=$1; cd /tmp/ft; pkill -9 -x $BIN 2>/dev/null
rm -rf usb0 usb1 log; mkdir -p usb0/torrents/downloads/Chk log; ln src/Chk/big.bin usb0/torrents/downloads/Chk/big.bin; cp torrents_src/Chk.torrent usb0/torrents/
setsid ./$BIN > chk_$BIN.out 2>&1 < /dev/null &
sleep 0.4; PID=$(pgrep -x $BIN | head -1)
T0=$(date +%s.%N); C0=$(awk '{print $14+$15}' /proc/$PID/stat)
for i in $(seq 1 300); do st=$(curl -s -m 2 localhost:18787/status | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['items'][0]['status'] if d['items'] else 'none')" 2>/dev/null); [ "$st" = "complete" ] && break; sleep 0.2; done
T1=$(date +%s.%N); C1=$(awk '{print $14+$15}' /proc/$PID/stat)
python3 -c "
w=$T1-$T0; c=($C1-$C0)/100.0
print('%-11s check of 400 MB took %5.1f s  (%5.0f MB/s)   CPU used %5.1f s  = %3.0f%% of one core' % ('$BIN', w, 400/w, c, 100*c/w))"
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 2; pkill -9 -x $BIN 2>/dev/null
