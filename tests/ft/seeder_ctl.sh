#!/bin/bash
# usage: seeder_ctl.sh start [delay] | stop
case "$1" in
  stop) for p in $(pgrep -f "python3 /tmp/ft/seeder_main"); do kill $p 2>/dev/null; done; sleep 0.5 ;;
  start) cp /tmp/ft/seeder.py /tmp/ft/seeder_main.py
         DELAY=${2:-0} STALL_AFTER=${3:-0} STALL_SECONDS=${4:-0} BURST=${5:-0} TRACKER_DELAY=${6:-0} setsid python3 /tmp/ft/seeder_main.py > /tmp/ft/seeder.log 2>&1 < /dev/null &
         sleep 1.5 ;;
esac
