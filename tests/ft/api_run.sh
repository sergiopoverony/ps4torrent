#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_host 2>/dev/null
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0.01 0 0 0
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; echo "web_token=sekret" > log/config.txt
setsid ./daemon_host > api.out 2>&1 < /dev/null &
sleep 2
timeout 150 python3 api_test.py > api.result 2>&1
echo "--- second start: settings must persist across a restart" >> api.result
curl -s -m 3 "localhost:18787/quit?token=sekret" >/dev/null; sleep 3
setsid ./daemon_host > api2.out 2>&1 < /dev/null &
sleep 3
curl -s -m 3 "localhost:18787/api/config?token=sekret" >> api.result; echo >> api.result
curl -s -m 3 "localhost:18787/quit?token=sekret" >/dev/null; sleep 3
echo "sanitizer run1: [$(grep -v notify api.out | head -3)]" >> api.result
echo "sanitizer run2: [$(grep -v notify api2.out | head -3)]" >> api.result
echo done >> api.result
