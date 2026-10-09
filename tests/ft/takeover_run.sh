#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_net2 2>/dev/null; rm -f netmock accept_fail stall_main incoming_accept_fail slow_io ignore_quit_flag log/quit.flag
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0.05 0 0 0 0
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Small.torrent torrents_src/Big8M.torrent usb0/torrents/
timeout 150 python3 takeover_test.py > takeover.result 2>&1
pkill -9 -x daemon_net2 2>/dev/null; rm -f stall_main ignore_quit_flag; bash seeder_ctl.sh stop
for f in copyA copyB copyC copyD; do echo "sanitizer $f: [$(grep -v notify $f.out 2>/dev/null | head -3)]" >> takeover.result; done
echo done >> takeover.result
