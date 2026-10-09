#!/bin/bash
cd /tmp/ft; rm -f sel.out; timeout 330 python3 sel_test.py > sel.result 2>&1
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 3; pkill -9 -x daemon_sel 2>/dev/null; bash seeder_ctl.sh stop
echo "sanitizer: [$(grep -v '^\[notify\]' sel.out | head -4)]" >> sel.result; echo done >> sel.result
