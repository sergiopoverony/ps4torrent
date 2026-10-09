#!/bin/bash
cd /tmp/ft; rm -f mem.out; timeout 280 python3 mem_test.py > mem.result 2>&1
pkill -9 -x daemon_mem 2>/dev/null; pkill -9 -x daemon_old 2>/dev/null; pkill -9 -x daemon_mem_plain 2>/dev/null; pkill -9 -x daemon_old_plain 2>/dev/null; bash seeder_ctl.sh stop
echo "sanitizer (new, ASAN build): [$(grep -v '^\[notify\]' mem.out | grep -E 'ERROR|runtime error|SUMMARY' | head -3)]" >> mem.result; echo done >> mem.result
