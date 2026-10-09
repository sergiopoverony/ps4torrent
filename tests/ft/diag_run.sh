#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_diag 2>/dev/null; rm -f probe_hang free_mock write_cost storage_fail
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0 0 0 0 0
rm -rf usb0 usb0_gone usb1 internal log mnt_mock dev_mock; mkdir -p usb0/torrents log mnt_mock/usb0 dev_mock; touch dev_mock/da0 dev_mock/da0s1 dev_mock/null dev_mock/tty
cp torrents_src/Big8M.torrent usb0/torrents/
setsid ./daemon_diag > diag.out 2>&1 < /dev/null &
sleep 3
timeout 240 python3 diag_test.py > diag.result 2>&1
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 4; pkill -9 -x daemon_diag 2>/dev/null; rm -f probe_hang; bash seeder_ctl.sh stop
echo "sanitizer: [$(grep -v notify diag.out | head -3)]" >> diag.result; echo done >> diag.result
