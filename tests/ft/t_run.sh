#!/bin/bash
# t_run.sh: чистая флешка с Small.torrent и запуск daemon_crash
cd /tmp/ft; rm -rf usb0 usb1 log; mkdir -p usb1/torrents log; cp "torrents_src/Small.torrent" usb1/torrents/
setsid ./daemon_crash > crash.out 2>&1 < /dev/null &
sleep 2
