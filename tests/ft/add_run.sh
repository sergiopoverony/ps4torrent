#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_add 2>/dev/null; rm -f free_mock netmock write_cost storage_fail
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0 0 0 0 0
rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log
setsid ./daemon_add > add.out 2>&1 < /dev/null &
sleep 3
timeout 200 python3 add_test.py > add.result 2>&1
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 4; pkill -9 -x daemon_add 2>/dev/null; bash seeder_ctl.sh stop
echo "sanitizer: [$(grep -v notify add.out | head -3)]" >> add.result; echo done >> add.result
