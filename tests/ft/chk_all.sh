#!/bin/bash
cd /tmp/ft; rm -f chk.result
for round in 1 2; do for b in daemon_old daemon_new1; do bash chk_run.sh $b >> chk.result 2>&1; done; done
echo done >> chk.result
