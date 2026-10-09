#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_icon 2>/dev/null; rm -f free_mock netmock write_cost storage_fail icon.out
rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log; echo "web_token=sekret" > log/config.txt
setsid ./daemon_icon > icon.out 2>&1 < /dev/null &
sleep 3
timeout 120 python3 icon_test.py > icon.result 2>&1
pkill -9 -x daemon_icon 2>/dev/null
echo "sanitizer: [$(grep -v '^\[notify\]' icon.out | head -3)]" >> icon.result; echo done >> icon.result
