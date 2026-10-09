#!/bin/bash
cd /tmp/ft
for spec in "daemon_v8:v8" "daemon_v9:v9"; do
  BIN=${spec%%:*}; LB=${spec##*:}
  pkill -9 -x $BIN 2>/dev/null; rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Small.torrent usb0/torrents/
  setsid ./$BIN > idle_$LB.out 2>&1 < /dev/null &
  sleep 2; timeout 60 python3 idle_test.py $LB >> idle.result 2>&1
  curl -s -m 3 localhost:18787/quit >/dev/null; sleep 2; pkill -9 -x $BIN 2>/dev/null
done
echo done >> idle.result
