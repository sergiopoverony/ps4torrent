#!/bin/bash
# iocmp_run.sh <binary> <label>
BIN=$1; LB=$2; cd /tmp/ft; pkill -9 -x $BIN 2>/dev/null
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0.027 0 0 0 0         # ~600 КБ/с, как в вашем логе
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/
echo "1200 330" > write_cost                                            # медленная флешка: 1,2 с на вызов записи + ~3 МБ/с
setsid ./$BIN > iocmp_${LB}.out 2>&1 < /dev/null &
sleep 1.5
timeout 100 python3 iocmp.py "$LB" >> iocmp.result 2>&1
grep -c "slow main loop" log/log.txt | sed "s/^/   'slow main loop' lines in the log: /" >> iocmp.result
rm -f write_cost; curl -s -m 3 localhost:18787/quit >/dev/null; sleep 2; pkill -9 -x $BIN 2>/dev/null; bash seeder_ctl.sh stop
