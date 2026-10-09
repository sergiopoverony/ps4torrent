#!/bin/bash
cd /tmp/ft; timeout 250 python3 wr_test.py > wr.result 2>&1
pkill -9 -x daemon_host 2>/dev/null; rm -f write_cost; bash seeder_ctl.sh stop
echo "sanitizer: [$(grep -v notify wr.out | head -3)]" >> wr.result; echo done >> wr.result
