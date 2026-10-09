#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_net 2>/dev/null
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0 0 0 0 0
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Small.torrent usb0/torrents/; echo -n "" > netmock
setsid ./daemon_net > net.out 2>&1 < /dev/null &
sleep 3
timeout 200 python3 net_test.py > net.result 2>&1
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 3; rm -f netmock; bash seeder_ctl.sh stop
echo "sanitizer: [$(grep -v notify net.out | head -3)]" >> net.result; echo done >> net.result
