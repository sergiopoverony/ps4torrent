#!/bin/bash
# io_cmp.sh <binary> <label> : Big8M при скорости ~600 КБ/с на "медленной флешке"
BIN=$1; LABEL=$2; cd /tmp/ft; pkill -9 -x $BIN 2>/dev/null
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0.027 0 0 0 0
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/
echo "90 1000" > write_cost                     # 90 мс на вызов записи + 1 мс на КБ (около 1 МБ/с)
setsid ./$BIN > io_$LABEL.out 2>&1 < /dev/null &
sleep 3
timeout 120 python3 resp.py $LABEL 75 >> io_cmp.result 2>&1
echo "           slow-loop lines in the log: $(grep -c 'slow main loop' log/log.txt), longest: $(grep -o 'took [0-9]* ms' log/log.txt | awk '{print $2}' | sort -n | tail -1) ms; storage slow lines: $(grep -c 'storage: slow' log/log.txt)" >> io_cmp.result
rm -f write_cost; curl -s -m 3 localhost:18787/quit >/dev/null; sleep 3; pkill -9 -x $BIN 2>/dev/null; bash seeder_ctl.sh stop
