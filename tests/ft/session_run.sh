#!/bin/bash
cd /tmp/ft; timeout 270 python3 session_test.py > session.result 2>&1
pkill -9 -x daemon_sess 2>/dev/null; bash seeder_ctl.sh stop
echo "sanitizer: [$(grep -v notify sess.out | head -3)]" >> session.result; echo done >> session.result
