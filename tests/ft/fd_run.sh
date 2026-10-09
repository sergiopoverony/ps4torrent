#!/bin/bash
cd /tmp/ft; timeout 280 python3 fd_test.py > fd.result 2>&1
pkill -9 -x daemon_net2 2>/dev/null; rm -f storage_fail stall_main netmock; bash seeder_ctl.sh stop
echo "sanitizer: [$(grep -v notify fd.out | head -3)]" >> fd.result; echo done >> fd.result
