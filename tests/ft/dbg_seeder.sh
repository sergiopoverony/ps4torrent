#!/bin/bash
cd /tmp/ft
bash seeder_ctl.sh stop; sleep 1
echo "leftover seeder processes after stop:"; ps -eo pid,args | grep "seeder_ma[i]n" | head
bash seeder_ctl.sh start 0.05 0 0 0 0 4
sleep 1
echo "seeder.log:"; head -5 seeder.log
echo "seeder processes now:"; ps -eo pid,args | grep "seeder_ma[i]n" | head
echo -n "tracker answer time: "; curl -s -o /dev/null -w "%{time_total}s\n" -m 10 "http://127.0.0.1:16969/announce?x=1"
