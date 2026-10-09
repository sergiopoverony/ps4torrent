#!/bin/bash
# usage: daemon_ctl.sh start | quit | kill9
case "$1" in
  start) cd /tmp/ft; setsid ./daemon_host > /tmp/ft/daemon.out 2>&1 < /dev/null & sleep 1 ;;
  quit)  curl -s localhost:18787/quit; sleep 3 ;;
  kill9) pkill -9 -x daemon_host; sleep 0.5 ;;
esac
