#!/bin/bash
cd /tmp/daemon
cp /tmp/pkgtest/fakeA.pkg /tmp/pkgtest/fakeB.pkg /tmp/pkgtest/notpkg.bin /tmp/daemon/ 2>/dev/null
SRC="main.cpp pkgmgr.cpp incoming.cpp log.cpp net.cpp bencode.cpp sha1.cpp torrent.cpp tracker.cpp storage.cpp download.cpp resume.cpp swarm.cpp blob.S"
COMMON="-std=c++17 -O1 -g -Wall -fsanitize=address,undefined -DHOST_TEST -DLISTEN_PORT=26881 -DHAVE_PKG -DPKG_START_DELAY_S=0 -DPKG_POLL_S=1 -DPKG_WATCH_MAX_S=25 -DPKG_AUTO_WAIT_S=8"
for v in fakeA:pkgA fakeB:pkgB notpkg:pkgBad; do
  f=${v%%:*}; n=${v##*:}; ext=pkg; [ $f = notpkg ] && ext=bin
  g++ $COMMON -DBLOB_PATH=\"$f.$ext\" -DUSB_BASE='"/tmp/ft/usb"' -DHTTP_PORT=18787 -DLOG_DIR='"/tmp/ft/log"' -DCONFIG_FILE='"/tmp/ft/log/config.txt"' \
      -DPKG_APP_DIR='"/tmp/ft/pkgmock/user_app"' -DPKG_DROP_DIR='"/tmp/ft/pkgmock/data_pkg"' -DPKG_STATE_DIR='"/tmp/ft/log"' -DPKG_MOCK_DIR='"/tmp/ft/pkgmock"' \
      -o /tmp/ft/daemon_$n $SRC -lpthread 2>&1 | head -5
  echo "built $n rc=$?" >> /tmp/ft/pkg_build.result
done
echo done >> /tmp/ft/pkg_build.result
