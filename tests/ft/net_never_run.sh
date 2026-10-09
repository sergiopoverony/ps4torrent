#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_net 2>/dev/null
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Small.torrent usb0/torrents/; echo -n "" > netmock
setsid ./daemon_net > netn.out 2>&1 < /dev/null &
sleep 2
timeout 100 python3 net_never.py > net_never.result 2>&1
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 3; rm -f netmock
echo "sanitizer: [$(grep -v notify netn.out | head -3)]" >> net_never.result; echo done >> net_never.result
