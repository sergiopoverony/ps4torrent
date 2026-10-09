#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_net2 2>/dev/null; rm -f netmock accept_fail stall_main incoming_accept_fail slow_io ignore_quit_flag
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0.05 0 0 0 0
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Small.torrent torrents_src/Big8M.torrent usb0/torrents/
setsid ./daemon_net2 > heal.out 2>&1 < /dev/null &
sleep 3
timeout 120 python3 heal_test.py > heal.result 2>&1
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 3; rm -f accept_fail stall_main incoming_accept_fail; bash seeder_ctl.sh stop
echo "sanitizer: [$(grep -v notify heal.out | head -3)]" >> heal.result
echo "notifications about the web address (periodic refreshes must NOT add more): $(grep -c 'ps4torrent web:' heal.out)" >> heal.result; echo done >> heal.result
