#!/bin/bash
cd /tmp/ft; timeout 250 python3 fix_test.py > fix.result 2>&1
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 3; pkill -9 -x daemon_fix 2>/dev/null; rm -f write_cost storage_fail stall_main; bash seeder_ctl.sh stop
echo "sanitizer: [$(grep -v notify fix.out | head -3)]" >> fix.result; echo done >> fix.result
