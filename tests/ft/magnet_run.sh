#!/bin/bash
# Magnet-запросы (бинарник daemon_host). Результат: /tmp/ft/magnet.result
cd /tmp/ft; pkill -9 -x daemon_host 2>/dev/null; rm -f free_mock netmock write_cost storage_fail
rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log internal/torrents
setsid ./daemon_host > magnet.out 2>&1 < /dev/null &
sleep 3
timeout 240 python3 magnet_test.py > magnet.result 2>&1
pkill -9 -x daemon_host 2>/dev/null
echo "sanitizer: [$(grep -v notify magnet.out | head -3)]" >> magnet.result; echo done >> magnet.result
