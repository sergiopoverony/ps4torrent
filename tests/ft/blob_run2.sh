#!/bin/bash
cp /tmp/ft/blob_info.h.good /tmp/daemon/blob_info.h
cd /tmp/daemon && g++ -std=c++17 -O1 -g -Wall -fsanitize=address,undefined -DHOST_TEST -DLISTEN_PORT=26881 -DHAVE_BLOB -DBLOB_PATH=\"blob.bin\" -DUSB_BASE='"/tmp/ft/usb"' -DHTTP_PORT=18787 -DLOG_DIR='"/tmp/ft/log"' -DCONFIG_FILE='"/tmp/ft/log/config.txt"' -o /tmp/ft/daemon_blob main.cpp incoming.cpp log.cpp net.cpp bencode.cpp sha1.cpp torrent.cpp tracker.cpp storage.cpp download.cpp resume.cpp swarm.cpp blob.S -lpthread 2>&1 | head -3
{
for scenario in A C; do
  cd /tmp/ft; pkill -9 -x daemon_blob 2>/dev/null; rm -rf usb0 usb1 log; mkdir -p usb0/torrents log
  [ $scenario = C ] && mkdir -p log/blob_test.bin
  setsid ./daemon_blob > blob.out 2>&1 < /dev/null &
  sleep 4; echo "=== $scenario"; grep "blob:" log/log.txt
  curl -s -m 3 localhost:18787/quit >/dev/null; sleep 3
  grep notify blob.out; echo "sanitizer: [$(grep -v notify blob.out | head -3)]"
done
} > /tmp/ft/blob2.result 2>&1
echo done >> /tmp/ft/blob2.result
