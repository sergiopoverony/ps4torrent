#!/bin/bash
# inbound_ctl.sh start <TORRENT> [DELAY] | stop
case "$1" in
  stop)
    if [ -f /tmp/ft/inbound.pid ]; then kill $(cat /tmp/ft/inbound.pid) 2>/dev/null; rm -f /tmp/ft/inbound.pid; fi
    sleep 0.5 ;;
  start)
    TORRENT=${2:-Big8M} DELAY=${3:-0} setsid python3 /tmp/ft/inbound.py > /tmp/ft/inbound.log 2>&1 < /dev/null &
    echo $! > /tmp/ft/inbound.pid
    sleep 0.5 ;;
esac
