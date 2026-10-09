#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_auto 2>/dev/null
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0 0 0 0 0
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log
cp torrents_src/Big8M.torrent torrents_src/Small.torrent "torrents_src/Game One.torrent" torrents_src/Many.torrent usb0/torrents/
setsid ./daemon_auto > auto.out 2>&1 < /dev/null &
sleep 3
timeout 150 python3 auto_test.py > auto.result 2>&1
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 3
echo "sanitizer: [$(grep -v notify auto.out | head -3)]" >> auto.result; echo done >> auto.result
