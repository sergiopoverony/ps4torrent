#!/bin/bash
# lat_run.sh <binary> <label> <tracker_delay>
BIN=$1; LABEL=$2; TD=${3:-0}
cd /tmp/ft; pkill -9 -x $(basename $BIN) 2>/dev/null
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0.05 0 0 0 $TD
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/
setsid $BIN > lat_$LABEL.out 2>&1 < /dev/null &
sleep 1
timeout 150 python3 latency.py $LABEL > lat_$LABEL.result 2>&1
timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 2; pkill -9 -x $(basename $BIN) 2>/dev/null; echo done >> lat_$LABEL.result
