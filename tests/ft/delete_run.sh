#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_host 2>/dev/null
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0 0 0 0
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log
cp "torrents_src/Game One.torrent" torrents_src/GameOnePart.torrent torrents_src/Small.torrent torrents_src/Evil.torrent usb0/torrents/
setsid ./daemon_host > delete.out 2>&1 < /dev/null &
sleep 2
timeout 120 python3 delete_test.py > delete.result 2>&1
timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 3
echo "sanitizer: [$(grep -v notify delete.out | head -3)]" >> delete.result
echo "log lines about delete:" >> delete.result; grep -E "delete|removed" log/log.txt | head -12 >> delete.result
echo done >> delete.result
