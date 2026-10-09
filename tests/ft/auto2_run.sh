#!/bin/bash
cd /tmp/ft
timeout 250 python3 auto2.py > auto2.result 2>&1
bash seeder_ctl.sh stop
for f in s1 s2 s4; do echo "sanitizer $f: [$(grep -v notify $f.out 2>/dev/null | head -3)]" >> auto2.result; done
echo done >> auto2.result
