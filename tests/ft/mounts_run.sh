#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_mnt 2>/dev/null; rm -f free_mock netmock write_cost storage_fail
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0 0 0 0 0
rm -rf usb0 usb0_gone usb1 usb1_gone usb2 usb2.unmounted usb4 usb4.readonly usb5 internal log; mkdir -p usb0/torrents log usb1 usb2 usb4 usb5
touch usb2.unmounted usb4.readonly
setsid ./daemon_mnt > mnt.out 2>&1 < /dev/null &
sleep 3
timeout 150 python3 mounts_test.py > mounts.result 2>&1
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 4; pkill -9 -x daemon_mnt 2>/dev/null; bash seeder_ctl.sh stop
rm -rf usb0_gone usb1_gone usb2 usb2.unmounted usb4 usb4.readonly usb5 usb1
echo "sanitizer: [$(grep -v '^\[notify\]' mnt.out | head -3)]" >> mounts.result; echo done >> mounts.result
