#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_store 2>/dev/null; rm -f free_mock netmock write_cost storage_fail
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0 0 0 0 0
rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log
setsid ./daemon_store > store.out 2>&1 < /dev/null &
sleep 3
timeout 280 python3 storage_test.py > storage.result 2>&1
echo "--- restart: the choice must persist" >> storage.result
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 4
setsid ./daemon_store > store2.out 2>&1 < /dev/null &
sleep 4
echo "save_to after restart: $(curl -s -m 3 localhost:18787/api/config | python3 -c 'import sys,json; print(json.load(sys.stdin)["save_to"])')" >> storage.result
echo "console memory still listed: $(curl -s -m 3 localhost:18787/status | python3 -c 'import sys,json; print([x["kind"] for x in json.load(sys.stdin)["drives"]])')" >> storage.result
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 4; pkill -9 -x daemon_store 2>/dev/null; bash seeder_ctl.sh stop; rm -f free_mock
echo "notifications about low space: $(grep -c 'LOW FREE SPACE\|NOT starting' store.out)" >> storage.result
echo "sanitizer: [$(grep -v notify store.out | head -3)] [$(grep -v notify store2.out | head -3)]" >> storage.result
echo done >> storage.result
