#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_cap_new 2>/dev/null; pkill -9 -x daemon_host 2>/dev/null
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0 0 0 0
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log
cp torrents_src/Small.torrent bigtorrents/files10000.torrent bigtorrents/files1000.torrent usb0/torrents/
setsid ./daemon_cap_new > big.out 2>&1 < /dev/null &
sleep 8
{
echo "== scanning a folder with a 10000-file torrent (cannot fit the 10 MB heap) next to a normal one"
grep -E "bad torrent|item:" log/log.txt | cut -c1-170
timeout 5 curl -s -m 3 localhost:18787/status | python3 -c "import sys,json; d=json.load(sys.stdin); [print('  ', i['title'][:40], i['status']) for i in d['items']]"
echo "daemon alive: $(pgrep -x daemon_cap_new >/dev/null && echo yes || echo NO)"
grep -E "EXCEPTION|FATAL" log/log.txt | head -2
echo "== uploading the same big torrent through the web API"
curl -s -m 10 -X POST --data-binary @bigtorrents/files10000.torrent "localhost:18787/api/add?name=huge-list.torrent"; echo
echo "daemon alive after upload: $(pgrep -x daemon_cap_new >/dev/null && echo yes || echo NO); leftover temp file: $(ls usb0/torrents/.upload.tmp 2>/dev/null || echo none)"
echo "== a torrent file bigger than 2 MB on the drive is skipped by size"
head -c 3000000 /dev/zero > usb0/torrents/huge-garbage.torrent; sleep 12
grep -E "huge-garbage" log/log.txt | cut -c1-160
timeout 3 curl -s -m 2 localhost:18787/quit >/dev/null; sleep 3
grep MEMCAP big.out
} > bigtorrent.result 2>&1
echo done >> bigtorrent.result
