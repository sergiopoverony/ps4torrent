#!/bin/bash
cd /tmp/ft; timeout 230 python3 done_test.py > done.result 2>&1
pkill -9 -x daemon_done 2>/dev/null; bash seeder_ctl.sh stop
echo "sanitizer: [$(grep -v '^\[notify\]' done.out | head -4)]" >> done.result; echo done >> done.result
