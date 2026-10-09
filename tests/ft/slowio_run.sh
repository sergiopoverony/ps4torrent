#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_net2 2>/dev/null; rm -f netmock accept_fail stall_main incoming_accept_fail ignore_quit_flag
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0.005 0 0 0 0
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent usb0/torrents/
echo 1200 > slow_io
setsid ./daemon_net2 > slowio.out 2>&1 < /dev/null &
sleep 22
{
echo "slow-I/O lines in the log (each storage call takes 1.2 s in this test; at most one line per 5 s):"
grep "storage: slow" log/log.txt | head -5
echo "count: $(grep -c 'storage: slow' log/log.txt) lines in ~20 s (rate limit works if <= 5)"
echo "web status still answers while the disk is slow: $(curl -s -m 3 localhost:18787/status | head -c 30)"
} > slowio.result 2>&1
rm -f slow_io; curl -s -m 3 localhost:18787/quit >/dev/null; sleep 4; pkill -9 -x daemon_net2 2>/dev/null; bash seeder_ctl.sh stop
echo "sanitizer: [$(grep -v notify slowio.out | head -3)]" >> slowio.result; echo done >> slowio.result
