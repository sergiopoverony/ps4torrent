#!/bin/bash
cd /tmp/ft; timeout 400 python3 memguard_test.py daemon_memg > memg.result 2>&1; cat memg.result | grep -v "^PASS"
