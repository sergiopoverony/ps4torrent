#!/bin/bash
cd /tmp/daemon
BUILD() { g++ -std=c++17 -O1 -g -Wall -fsanitize=address,undefined -DHOST_TEST -DLISTEN_PORT=26881 -DHAVE_BLOB -DBLOB_PATH=\"blob.bin\" -DUSB_BASE='"/tmp/ft/usb"' -DHTTP_PORT=18787 -DLOG_DIR='"/tmp/ft/log"' -DCONFIG_FILE='"/tmp/ft/log/config.txt"' -o /tmp/ft/daemon_blob main.cpp incoming.cpp log.cpp net.cpp bencode.cpp sha1.cpp torrent.cpp tracker.cpp storage.cpp download.cpp resume.cpp swarm.cpp blob.S -lpthread 2>&1 | head -5; }
RUN() { cd /tmp/ft; pkill -9 -x daemon_blob 2>/dev/null; rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; [ -n "$1" ] && eval "$1"; setsid ./daemon_blob > blob.out 2>&1 < /dev/null & sleep 4; }
{
echo "=== A: normal"; cp blob_info.h blob_info.h.good; BUILD; RUN ""
grep "blob:" /tmp/ft/log/log.txt; grep notify /tmp/ft/blob.out
ls -la /tmp/ft/log/blob_test.bin | awk '{print "file on disk:", $5, "bytes"}'; cmp -s /tmp/ft/log/blob_test.bin /tmp/daemon/blob.bin && echo "file on disk IDENTICAL to blob.bin" || echo "file on disk DIFFERENT"
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 3; echo "sanitizer: [$(grep -v notify /tmp/ft/blob.out | head -3)]"
echo; echo "=== B: wrong expected hash (simulates damaged data)"
cd /tmp/daemon; echo '#define BLOB_SHA1_HEX "0000000000000000000000000000000000000000"' > blob_info.h; BUILD; RUN ""
grep "blob:" /tmp/ft/log/log.txt; grep notify /tmp/ft/blob.out | tail -1
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 3
echo; echo "=== C: the disk file cannot be created (a directory has that name) - daemon must keep running"
cp /tmp/daemon/blob_info.h.good /tmp/daemon/blob_info.h 2>/dev/null; cp /tmp/ft/blob_info.h.good /tmp/daemon/blob_info.h; BUILD; RUN 'mkdir -p log/blob_test.bin'
grep "blob:" /tmp/ft/log/log.txt; grep notify /tmp/ft/blob.out | tail -1
echo "web still answers: $(curl -s -m 3 localhost:18787/status | head -c 40)"
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 3; echo "sanitizer: [$(grep -v notify /tmp/ft/blob.out | head -3)]"
cp /tmp/ft/blob_info.h.good /tmp/daemon/blob_info.h
} > /tmp/ft/blob.result 2>&1
echo done >> /tmp/ft/blob.result
